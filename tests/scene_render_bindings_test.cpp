// tests/scene_render_bindings_test.cpp -- task 3.1.5: buildRenderView's asset-resolution arm (BR*).
//
// Tier 0 (no GPU, every lane, BR1-BR20): the three arms of the emission walk, the material resolution
// order, the two new RenderView counts, and -- BR1 -- INV-D3, the null-table equivalence every caller
// written before 3.1.5 depends on. tests/scene_render_test.cpp is INV-D3's broad witness and is
// deliberately UNTOUCHED by this task; BR1 is the narrow, direct one.
//
// Tier 1 (a real Device, NO window -- RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL,
// BR21-BR22): that draw() ignores the two counts, and the whole chain from a cook in memory to a
// SceneRenderer draw with a resolved reference.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md). Enum comparisons use
// double parentheses, for the same ADL reason render_skinning_test.cpp states.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/render/render.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene_render/asset_bindings.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::Guid;
using engine::Mat4;
using engine::MeshRenderer;
using engine::PointLight;
using engine::Quat;
using engine::SpotLight;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::render::MaterialHandle;
using engine::render::MeshHandle;
using engine::render::MeshInstance;
using engine::render::PrimitiveId;
using engine::render::RenderView;
using engine::scene_render::AssetBindingTable;
using engine::scene_render::buildRenderView;
using engine::scene_render::MeshBinding;
using engine::scene_render::MeshBindingSubmesh;
using engine::scene_render::RenderViewScratch;

namespace {

constexpr engine::rhi::Extent2D VIEWPORT{1920U, 1080U};

[[nodiscard]] Guid guidOf(std::uint64_t ordinal) { return Guid{ordinal, 0xA55EULL}; }

// A camera at a fixed place, so every case below reaches the light walk and the mvp loop rather than
// the 0-camera early return (which BR17 exercises on purpose).
Entity addCamera(World& world) {
    const Entity e = world.create();
    REQUIRE(world.add<Transform>(e, Transform{Vec3{0.0F, 1.0F, 4.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(world.add<Camera>(e, Camera{}) != nullptr);
    return e;
}

Entity addMeshEntity(World& world, const Transform& transform, const MeshRenderer& meshRenderer) {
    const Entity e = world.create();
    REQUIRE(world.add<Transform>(e, transform) != nullptr);
    REQUIRE(world.add<MeshRenderer>(e, meshRenderer) != nullptr);
    return e;
}

// `sourceMeshIndices[i]` is submesh i's D2 join key; every submesh gets an INVALID material unless
// `materials` says otherwise.
[[nodiscard]] MeshBinding bindingOf(MeshHandle mesh, const std::vector<std::uint32_t>& sourceMeshIndices,
                                    const std::vector<MaterialHandle>& materials = {}) {
    MeshBinding binding;
    binding.mesh = mesh;
    for (std::size_t i = 0; i < sourceMeshIndices.size(); ++i) {
        MeshBindingSubmesh sub;
        sub.submesh = static_cast<std::uint32_t>(i);
        sub.sourceMeshIndex = sourceMeshIndices[i];
        sub.material = i < materials.size() ? materials[i] : MaterialHandle{};
        binding.submeshes.push_back(sub);
    }
    return binding;
}

// Field for field, including the two members arm 1 never sets. Used by BR1, where "equivalent" has to
// mean every observable rather than "the same count".
[[nodiscard]] bool sameInstance(const MeshInstance& a, const MeshInstance& b) {
    return a.primitive == b.primitive && a.mesh == b.mesh && a.submesh == b.submesh &&
           a.palette.data() == b.palette.data() && a.palette.size() == b.palette.size() && a.mvp == b.mvp &&
           a.model == b.model && a.normalMatrix == b.normalMatrix && a.color == b.color && a.material == b.material;
}

// MEASURED, not assumed: World::each's ENTITY order is EnTT's view order, which is not creation order
// and is no part of buildRenderView's contract. Every multi-entity case here therefore identifies its
// instances by a distinguishing field instead of by position. WITHIN one entity the order IS specified
// -- cooked submesh order, the D2 join key's own -- and those cases assert it directly and positionally
// (BR5, BR7), which is the property S13 has to break.
[[nodiscard]] std::vector<MeshInstance> instancesColored(const RenderView& view, Vec3 color) {
    std::vector<MeshInstance> matching;
    for (const MeshInstance& instance : view.instances) {
        if (instance.color == color) {
            matching.push_back(instance);
        }
    }
    return matching;
}

}  // namespace

// ================================================================================================
// INV-D3 -- the null-table equivalence
// ================================================================================================

TEST_CASE("scene_render bindings: a defaulted call and an explicit null table agree field for field (BR1)") {
    World world;
    addCamera(world);
    // Every pre-3.1.5 shape in one world: three primitives (one out of range), a non-uniform scale, a
    // directional light and two point lights. None of them can carry a reference, which is exactly why
    // INV-D3's second half is unreachable for a pre-3.1.5 input.
    addMeshEntity(world, Transform{Vec3{-1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()},
                  MeshRenderer{.primitive = 0, .color = Vec3{0.9F, 0.1F, 0.2F}});
    addMeshEntity(world,
                  Transform{Vec3{1.0F, 0.0F, 0.0F}, engine::fromAxisAngle(Vec3::unitY(), engine::radians(30.0F)),
                            Vec3{2.0F, 0.5F, 1.0F}},
                  MeshRenderer{.primitive = 1, .color = Vec3{0.1F, 0.9F, 0.2F}});
    addMeshEntity(world, Transform{Vec3{0.0F, 2.0F, 0.0F}, Quat::identity(), Vec3::one()},
                  MeshRenderer{.primitive = 99, .color = Vec3::one()});  // out of range -> Cube
    {
        const Entity light = world.create();
        REQUIRE(world.add<Transform>(light) != nullptr);
        REQUIRE(world.add<DirectionalLight>(light, DirectionalLight{Vec3::one(), 1.0F}) != nullptr);
    }
    {
        const Entity point = world.create();
        REQUIRE(world.add<Transform>(point, Transform{Vec3{3.0F, 3.0F, 3.0F}, Quat::identity(), Vec3::one()}) !=
                nullptr);
        REQUIRE(world.add<PointLight>(point, PointLight{Vec3::one(), 2.0F, 9.0F}) != nullptr);
    }
    {
        // task E.2.2 -- a spot beside the point, so the two calls' agreement covers the spot span too.
        const Entity spot = world.create();
        REQUIRE(world.add<Transform>(spot, Transform{Vec3{4.0F, 4.0F, 4.0F}, Quat::identity(), Vec3::one()}) !=
                nullptr);
        REQUIRE(world.add<SpotLight>(spot, SpotLight{Vec3::one(), 2.0F, 9.0F, 0.2F, 0.4F}) != nullptr);
    }

    RenderViewScratch defaultedScratch;
    RenderViewScratch explicitScratch;
    const RenderView defaulted = buildRenderView(world, defaultedScratch, VIEWPORT);
    const RenderView explicitNull = buildRenderView(world, explicitScratch, VIEWPORT, nullptr, nullptr);

    REQUIRE(defaulted.instances.size() == 3);
    REQUIRE(explicitNull.instances.size() == defaulted.instances.size());
    for (std::size_t i = 0; i < defaulted.instances.size(); ++i) {
        CHECK(sameInstance(defaulted.instances[i], explicitNull.instances[i]));
    }
    CHECK(defaulted.hasCamera == explicitNull.hasCamera);
    CHECK(defaulted.cameraCount == explicitNull.cameraCount);
    CHECK(defaulted.directionalCount == explicitNull.directionalCount);
    CHECK(defaulted.pointsTruncated == explicitNull.pointsTruncated);
    CHECK(defaulted.points.size() == explicitNull.points.size());
    CHECK(defaulted.spotsTruncated == explicitNull.spotsTruncated);  // task E.2.2
    CHECK(defaulted.spots.size() == explicitNull.spots.size());
    CHECK(defaulted.spots.size() == 1);  // anti-vacuity: the spot entity above was resolved at all
    // The two new counts stay 0 on every pre-3.1.5 input, on BOTH calls.
    CHECK(defaulted.unresolvedMeshes == 0);
    CHECK(defaulted.unresolvedMaterials == 0);
    CHECK(explicitNull.unresolvedMeshes == 0);
    CHECK(explicitNull.unresolvedMaterials == 0);
}

// ================================================================================================
// arm 1 -- the primitive path
// ================================================================================================

TEST_CASE("scene_render bindings: a NIL mesh reference emits one primitive instance, clamped (BR2)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 2, .color = Vec3{0.2F, 0.4F, 0.6F}});
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 7, .color = Vec3{0.9F, 0.8F, 0.7F}});  // >= Count

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0}));  // present but irrelevant: `mesh` is nil

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);

    const std::vector<MeshInstance> plane = instancesColored(view, Vec3{0.2F, 0.4F, 0.6F});
    const std::vector<MeshInstance> clamped = instancesColored(view, Vec3{0.9F, 0.8F, 0.7F});
    REQUIRE(plane.size() == 1);
    REQUIRE(clamped.size() == 1);
    CHECK((plane[0].primitive == PrimitiveId::Plane));
    CHECK((clamped[0].primitive == PrimitiveId::Cube));  // out of range clamps, never wraps
    CHECK_FALSE(plane[0].mesh.valid());
    CHECK_FALSE(clamped[0].mesh.valid());
    CHECK(view.unresolvedMeshes == 0);
    CHECK(view.unresolvedMaterials == 0);
}

// ================================================================================================
// arm 2 -- a reference with nothing to resolve it
// ================================================================================================

TEST_CASE("scene_render bindings: a valid reference with a NULL table draws nothing and counts once (BR3)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    CHECK(view.instances.empty());
    CHECK(view.unresolvedMeshes == 1);
    CHECK(view.unresolvedMaterials == 0);
}

TEST_CASE("scene_render bindings: a valid reference with an EMPTY table behaves the same (BR4)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});

    const AssetBindingTable table;
    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    CHECK(view.instances.empty());
    CHECK(view.unresolvedMeshes == 1);
}

// ================================================================================================
// arm 3 -- resolved
// ================================================================================================

TEST_CASE("scene_render bindings: a resolved reference emits ONE instance PER MATCHING SUBMESH (BR5)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(3), .meshIndex = 0});

    AssetBindingTable table;
    table.setMesh(guidOf(3), bindingOf(MeshHandle{5, 2}, {0, 0, 0}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 3);
    CHECK(view.instances[0].submesh == 0U);
    CHECK(view.instances[1].submesh == 1U);
    CHECK(view.instances[2].submesh == 2U);
    for (const MeshInstance& instance : view.instances) {
        CHECK(instance.mesh == MeshHandle{5, 2});
    }
    CHECK(view.unresolvedMeshes == 0);
}

TEST_CASE("scene_render bindings: a STALE meshIndex emits nothing and counts as unresolved (BR6)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(3), .meshIndex = 1});

    AssetBindingTable table;
    table.setMesh(guidOf(3), bindingOf(MeshHandle{5, 2}, {0, 0, 0}));  // every submesh belongs to mesh 0

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    CHECK(view.instances.empty());
    CHECK(view.unresolvedMeshes == 1);
}

TEST_CASE("scene_render bindings: the sourceMeshIndex filter selects exactly its own submeshes (BR7)") {
    // A four-submesh container whose meshes interleave (0, 1, 0, 2). An emission that ignores the
    // filter draws all four; one that stops at the first mismatch draws one.
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(4), .meshIndex = 0});

    AssetBindingTable table;
    table.setMesh(guidOf(4), bindingOf(MeshHandle{9, 1}, {0, 1, 0, 2}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);
    CHECK(view.instances[0].submesh == 0U);  // in TABLE order, not sorted, not reversed
    CHECK(view.instances[1].submesh == 2U);
    CHECK(view.unresolvedMeshes == 0);
}

TEST_CASE("scene_render bindings: an emitted instance's matrices equal the primitive arm's (BR8)") {
    // Compared against arm 1 for the SAME transform rather than against a re-derivation of `embed`:
    // this asserts the two arms agree, which is the property a reader cares about, and it cannot be
    // satisfied by copying the implementation into the test.
    const Transform transform{Vec3{1.5F, -2.0F, 0.25F}, engine::fromAxisAngle(Vec3::unitZ(), engine::radians(37.0F)),
                              Vec3{3.0F, 0.5F, 2.0F}};
    World world;
    addCamera(world);
    const Entity primitiveEntity =
        addMeshEntity(world, transform, MeshRenderer{.primitive = 0, .color = Vec3{0.3F, 0.6F, 0.9F}});
    addMeshEntity(world, transform,
                  MeshRenderer{.primitive = 0, .color = Vec3{0.3F, 0.6F, 0.9F}, .mesh = guidOf(2), .meshIndex = 0});

    AssetBindingTable table;
    table.setMesh(guidOf(2), bindingOf(MeshHandle{1, 1}, {0}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);
    const MeshInstance& fromPrimitive = view.instances[0];
    const MeshInstance& fromReference = view.instances[1];
    CHECK(fromReference.model == fromPrimitive.model);
    CHECK(fromReference.normalMatrix == fromPrimitive.normalMatrix);
    CHECK(fromReference.mvp == fromPrimitive.mvp);
    // ...and the model really is the entity's world matrix, so the equality above is not two wrongs.
    CHECK(fromPrimitive.model == engine::worldMatrix(world, primitiveEntity));
}

TEST_CASE("scene_render bindings: the component's colour rides onto every emitted submesh (BR9)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3{0.11F, 0.22F, 0.33F}, .mesh = guidOf(1), .meshIndex = 0});

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{2, 1}, {0, 0}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);
    for (const MeshInstance& instance : view.instances) {
        CHECK(instance.color == Vec3{0.11F, 0.22F, 0.33F});
    }
}

// ================================================================================================
// the material resolution order -- the D7 order IS the specification
// ================================================================================================

TEST_CASE("scene_render bindings: a RESOLVING override wins on every submesh (BR10)") {
    World world;
    addCamera(world);
    addMeshEntity(
        world, Transform{},
        MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1), .meshIndex = 0, .material = guidOf(50)});

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0, 0, 0},
                                       {MaterialHandle{11, 1}, MaterialHandle{12, 1}, MaterialHandle{13, 1}}));
    table.setMaterial(guidOf(50), MaterialHandle{77, 3});

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 3);
    for (const MeshInstance& instance : view.instances) {
        CHECK(instance.material == MaterialHandle{77, 3});  // the literal, never "the override function"
    }
    CHECK(view.unresolvedMaterials == 0);
}

TEST_CASE("scene_render bindings: an UNRESOLVED override is counted and falls through (BR11)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0,
                               .color = Vec3::one(),
                               .mesh = guidOf(1),
                               .meshIndex = 0,
                               .material = guidOf(50)});  // named, but never bound

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0, 0}, {MaterialHandle{11, 1}, MaterialHandle{12, 1}}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);
    CHECK(view.instances[0].material == MaterialHandle{11, 1});  // the SUBMESH's own, not blanked
    CHECK(view.instances[1].material == MaterialHandle{12, 1});
    CHECK(view.unresolvedMaterials == 2);  // once per EMITTED submesh
}

TEST_CASE("scene_render bindings: a NIL override leaves the submesh handles alone (BR12)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1), .meshIndex = 0});

    AssetBindingTable table;
    // submesh 1's handle is deliberately INVALID: the source assigned it no material, which is legal
    // and resolves to ForwardRenderer::defaultMaterial() at draw time.
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0, 0}, {MaterialHandle{21, 4}, MaterialHandle{}}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 2);
    CHECK(view.instances[0].material == MaterialHandle{21, 4});
    CHECK_FALSE(view.instances[1].material.valid());
    CHECK(view.unresolvedMaterials == 0);  // a nil override is not an unresolved one
}

TEST_CASE("scene_render bindings: two entities on one guid emit two independent groups (BR13)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{Vec3{-2.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()},
                  MeshRenderer{.primitive = 0, .color = Vec3{1.0F, 0.0F, 0.0F}, .mesh = guidOf(1), .meshIndex = 0});
    addMeshEntity(world, Transform{Vec3{2.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()},
                  MeshRenderer{.primitive = 0, .color = Vec3{0.0F, 1.0F, 0.0F}, .mesh = guidOf(1), .meshIndex = 0});

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0, 0}));
    REQUIRE(table.meshCount() == 1);

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 4);

    const std::vector<MeshInstance> left = instancesColored(view, Vec3{1.0F, 0.0F, 0.0F});
    const std::vector<MeshInstance> right = instancesColored(view, Vec3{0.0F, 1.0F, 0.0F});
    REQUIRE(left.size() == 2);   // one group of two...
    REQUIRE(right.size() == 2);  // ...and a second, independent one
    CHECK(left[0].submesh == 0U);
    CHECK(left[1].submesh == 1U);
    CHECK(right[0].submesh == 0U);
    CHECK(right[1].submesh == 1U);
    CHECK(left[0].model == left[1].model);   // one entity's two submeshes share its transform...
    CHECK(left[0].model != right[0].model);  // ...and the two entities do not
    CHECK(left[0].mesh == right[0].mesh);    // both resolved through the SAME entry
    CHECK(table.meshCount() == 1);           // one entry served both
}

TEST_CASE("scene_render bindings: the material count is PER EMITTED SUBMESH, not per entity (BR14)") {
    World world;
    addCamera(world);
    addMeshEntity(
        world, Transform{},
        MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1), .meshIndex = 0, .material = guidOf(50)});

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0, 0, 0}));  // three matching submeshes, override unbound

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 3);
    CHECK(view.unresolvedMaterials == 3);  // 3, never 1 -- a seven-submesh model would read 7
}

// ================================================================================================
// the surrounding walk, re-asserted because the arm changed
// ================================================================================================

TEST_CASE("scene_render bindings: a MeshRenderer with no Transform is still excluded (BR15)") {
    World world;
    addCamera(world);
    const Entity orphan = world.create();
    REQUIRE(world.add<MeshRenderer>(
                orphan, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1), .meshIndex = 0}) !=
            nullptr);

    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0}));

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    CHECK(view.instances.empty());
    // ...and it is not counted either: each<Transform, MeshRenderer> never visits it at all.
    CHECK(view.unresolvedMeshes == 0);
}

TEST_CASE("scene_render bindings: unresolvedMeshes accumulates across entities (BR16)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(2)});
    addMeshEntity(world, Transform{},
                  MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(3), .meshIndex = 9});
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one()});  // arm 1: never counted

    AssetBindingTable table;
    table.setMesh(guidOf(3), bindingOf(MeshHandle{1, 1}, {0}));  // present, but meshIndex 9 matches nothing

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 1);  // only the primitive entity
    CHECK(view.unresolvedMeshes == 3);    // two arm-2 misses + one arm-3 zero-match
}

TEST_CASE("scene_render bindings: the 0-camera early return still fills the counts (BR17)") {
    World world;  // NO camera at all
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 1, .color = Vec3::one()});

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    CHECK_FALSE(view.hasCamera);
    CHECK(view.cameraCount == 0);
    REQUIRE(view.instances.size() == 1);  // the primitive entity -- instances are assigned on this path
    CHECK((view.instances[0].primitive == PrimitiveId::Sphere));
    CHECK(view.unresolvedMeshes == 1);  // ...and so is the count
}

TEST_CASE("scene_render bindings: cameraCount is unchanged by the presence of references (BR18)") {
    World world;
    addCamera(world);
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    CHECK(view.cameraCount == 2);
    CHECK(view.hasCamera);
    CHECK(view.unresolvedMeshes == 1);
}

TEST_CASE("scene_render bindings: directionalCount is unchanged by the presence of references (BR19)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});
    for (int i = 0; i < 2; ++i) {
        const Entity light = world.create();
        REQUIRE(world.add<Transform>(light) != nullptr);
        REQUIRE(world.add<DirectionalLight>(light, DirectionalLight{Vec3::one(), 1.0F}) != nullptr);
    }

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    CHECK(view.directionalCount == 2);
    CHECK(view.unresolvedMeshes == 1);
}

TEST_CASE("scene_render bindings: pointsTruncated is unchanged by the presence of references (BR20)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(1)});
    for (std::uint32_t i = 0; i < engine::render::MAX_POINT_LIGHTS + 1; ++i) {
        const Entity light = world.create();
        REQUIRE(world.add<Transform>(light) != nullptr);
        REQUIRE(world.add<PointLight>(light, PointLight{Vec3::one(), 1.0F, 5.0F}) != nullptr);
    }

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    CHECK(view.points.size() == engine::render::MAX_POINT_LIGHTS);
    CHECK(view.pointsTruncated);
    CHECK(view.unresolvedMeshes == 1);
}

// ================================================================================================
// task E.5.1 -- arm 1 resolves `material` through the SAME D7 order arm 3 uses (BR23-BR29)
// ================================================================================================

namespace {

// BR23's four primitives, built twice in the SAME creation order into two worlds that differ ONLY in
// the material GUID -- so any field that differs between the two worlds' instances moved because of
// the material. One is rotated AND non-uniformly scaled, so the matrices compared are not trivial.
void addFourPrimitives(World& world, const std::vector<Vec3>& colors, Guid material) {
    const Quat turned = engine::fromAxisAngle(Vec3::unitY(), engine::radians(40.0F));
    const Transform left{Vec3{-2.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()};
    const Transform rotatedScaled{Vec3{2.0F, 0.5F, -1.0F}, turned, Vec3{2.0F, 0.5F, 1.5F}};
    const Transform flattened{Vec3{0.0F, -1.0F, -2.0F}, Quat::identity(), Vec3{3.0F, 1.0F, 3.0F}};
    const Transform above{Vec3{0.0F, 2.0F, 0.0F}, Quat::identity(), Vec3::one()};
    addMeshEntity(world, left, MeshRenderer{.primitive = 0, .color = colors[0], .material = material});
    addMeshEntity(world, rotatedScaled, MeshRenderer{.primitive = 1, .color = colors[1], .material = material});
    addMeshEntity(world, flattened, MeshRenderer{.primitive = 2, .color = colors[2], .material = material});
    // 9 is >= PrimitiveId::Count: it clamps to Cube, and must carry the material all the same (E8)
    addMeshEntity(world, above, MeshRenderer{.primitive = 9, .color = colors[3], .material = material});
}

}  // namespace

TEST_CASE("scene_render bindings: a primitive whose material RESOLVES draws with it, and only it moved (BR23)") {
    const std::vector<Vec3> colors{Vec3{0.9F, 0.1F, 0.1F}, Vec3{0.1F, 0.9F, 0.1F}, Vec3{0.1F, 0.1F, 0.9F},
                                   Vec3{0.5F, 0.5F, 0.5F}};
    AssetBindingTable table;
    table.setMaterial(guidOf(50), MaterialHandle{7, 1});

    World withMaterial;
    addCamera(withMaterial);
    addFourPrimitives(withMaterial, colors, guidOf(50));
    World nilTwin;  // the SAME four entities, created in the same order, every `material` NIL
    addCamera(nilTwin);
    addFourPrimitives(nilTwin, colors, Guid{});

    RenderViewScratch scratchA;
    RenderViewScratch scratchB;
    const RenderView a = buildRenderView(withMaterial, scratchA, VIEWPORT, nullptr, &table);
    const RenderView b = buildRenderView(nilTwin, scratchB, VIEWPORT, nullptr, &table);
    REQUIRE(a.instances.size() == 4);
    REQUIRE(b.instances.size() == 4);
    CHECK(a.unresolvedMaterials == 0);  // a RESOLVING override is not an unresolved one
    CHECK(a.unresolvedMeshes == 0);
    CHECK(b.unresolvedMaterials == 0);

    for (std::size_t i = 0; i < colors.size(); ++i) {
        CAPTURE(i);
        const std::vector<MeshInstance> fromA = instancesColored(a, colors[i]);
        const std::vector<MeshInstance> fromB = instancesColored(b, colors[i]);
        REQUIRE(fromA.size() == 1);
        REQUIRE(fromB.size() == 1);
        const MeshInstance& withIt = fromA[0];
        const MeshInstance& withoutIt = fromB[0];
        CHECK((withIt.material == MaterialHandle{7, 1}));  // the bound handle, on every primitive
        CHECK_FALSE(withoutIt.material.valid());           // NIL: what the arm emitted before E.5.1
        // ...and NOTHING ELSE moved: every other field equals the NIL twin's, bit for bit.
        CHECK((withIt.primitive == withoutIt.primitive));
        CHECK((withIt.mesh == withoutIt.mesh));
        CHECK_FALSE(withIt.mesh.valid());
        CHECK(withIt.submesh == withoutIt.submesh);
        CHECK(withIt.model == withoutIt.model);
        CHECK(withIt.normalMatrix == withoutIt.normalMatrix);
        CHECK(withIt.mvp == withoutIt.mvp);
        CHECK(withIt.color == withoutIt.color);
        CHECK(withIt.palette.empty());
        CHECK(withoutIt.palette.empty());
    }
    // The primitive selector is honoured with a material on, including the clamp.
    CHECK((instancesColored(a, colors[0])[0].primitive == PrimitiveId::Cube));
    CHECK((instancesColored(a, colors[1])[0].primitive == PrimitiveId::Sphere));
    CHECK((instancesColored(a, colors[2])[0].primitive == PrimitiveId::Plane));
    CHECK((instancesColored(a, colors[3])[0].primitive == PrimitiveId::Cube));  // 9 clamps, never wraps
    // ANTI-VACUITY: the rotated, non-uniformly scaled entity has a non-trivial normal matrix, so the
    // matrix equalities above compared something.
    CHECK_FALSE(instancesColored(a, colors[1])[0].normalMatrix == Mat4::identity());
}

TEST_CASE("scene_render bindings: a primitive's UNRESOLVED material is counted once, draws the default (BR24)") {
    World world;
    addCamera(world);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 1, .color = Vec3::one(), .material = guidOf(51)});

    // NON-EMPTY on purpose: a mesh entry and a material under OTHER guids, so an implementation that
    // counts only when the table is empty -- or only when it is null -- is caught here.
    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0}));
    table.setMaterial(guidOf(50), MaterialHandle{7, 1});
    REQUIRE(table.materialCount() == 1);
    REQUIRE_FALSE(table.findMaterial(guidOf(51)).valid());

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 1);
    CHECK((view.instances[0].primitive == PrimitiveId::Sphere));
    CHECK_FALSE(view.instances[0].material.valid());  // INVALID -> the default material at draw time
    CHECK(view.unresolvedMaterials == 1);
    CHECK(view.unresolvedMeshes == 0);  // a material miss is never a mesh miss
}

TEST_CASE("scene_render bindings: with a NULL table a primitive's VALID material is still counted (BR25)") {
    const Quat tilted = engine::fromAxisAngle(Vec3::unitX(), engine::radians(20.0F));
    const Transform transform{Vec3{0.5F, 0.0F, -1.0F}, tilted, Vec3{1.0F, 2.0F, 1.0F}};
    const MeshRenderer named{.primitive = 2, .color = Vec3{0.3F, 0.3F, 0.3F}, .material = guidOf(52)};
    MeshRenderer nil = named;
    nil.material = Guid{};

    World world;
    addCamera(world);
    addMeshEntity(world, transform, named);
    World twin;  // the SAME entity with a NIL material: what the arm emitted for it before E.5.1
    addCamera(twin);
    addMeshEntity(twin, transform, nil);

    RenderViewScratch explicitScratch;
    RenderViewScratch defaultedScratch;
    RenderViewScratch twinScratch;
    const RenderView explicitNull = buildRenderView(world, explicitScratch, VIEWPORT, nullptr, nullptr);
    const RenderView defaulted = buildRenderView(world, defaultedScratch, VIEWPORT);
    const RenderView twinView = buildRenderView(twin, twinScratch, VIEWPORT, nullptr, nullptr);
    REQUIRE(explicitNull.instances.size() == 1);
    REQUIRE(defaulted.instances.size() == 1);
    REQUIRE(twinView.instances.size() == 1);

    // D4: COUNTED with no table at all -- the imported arm's rule, verbatim, on both spellings of "null".
    CHECK(explicitNull.unresolvedMaterials == 1);
    CHECK(defaulted.unresolvedMaterials == 1);
    CHECK(twinView.unresolvedMaterials == 0);
    // INV-D3 as amended: the INSTANCE is the NIL twin's, field for field -- only the counter moved.
    CHECK_FALSE(explicitNull.instances[0].material.valid());
    CHECK(sameInstance(explicitNull.instances[0], twinView.instances[0]));
    CHECK(sameInstance(defaulted.instances[0], explicitNull.instances[0]));
}

TEST_CASE("scene_render bindings: a primitive with a NIL material is byte-identical to the pre-E.5.1 arm (BR26)") {
    // The expected instance is written BY HAND, as the pre-fix arm built it, at PURE TRANSLATIONS: there
    // the normal matrix is exactly the identity, so no line here restates `embed` (BR8's principle -- a
    // test that copies the implementation cannot falsify it). The mvp is composed with the BRIDGE's own
    // association, (proj * view) * model, or the comparison would not be bit-exact.
    const Vec3 red{0.7F, 0.2F, 0.2F};
    const Vec3 green{0.2F, 0.7F, 0.2F};
    const Vec3 blue{0.2F, 0.2F, 0.7F};
    const Transform left{Vec3{-1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()};
    const Transform right{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()};
    const Transform back{Vec3{0.0F, 1.0F, -1.0F}, Quat::identity(), Vec3::one()};
    World world;
    addCamera(world);
    const Entity cube = addMeshEntity(world, left, MeshRenderer{.primitive = 0, .color = red});
    const Entity sphere = addMeshEntity(world, right, MeshRenderer{.primitive = 1, .color = green});
    const Entity clamped = addMeshEntity(world, back, MeshRenderer{.primitive = 5, .color = blue});  // 5 clamps

    // A POPULATED table -- two material entries and a mesh entry, none of which a NIL material names --
    // and a NULL one. Neither may change a single field or move a counter.
    AssetBindingTable populated;
    populated.setMaterial(guidOf(50), MaterialHandle{7, 1});
    populated.setMaterial(guidOf(51), MaterialHandle{8, 2});
    populated.setMesh(guidOf(1), bindingOf(MeshHandle{1, 1}, {0}));

    RenderViewScratch populatedScratch;
    RenderViewScratch nullScratch;
    const RenderView withTable = buildRenderView(world, populatedScratch, VIEWPORT, nullptr, &populated);
    const RenderView withoutTable = buildRenderView(world, nullScratch, VIEWPORT, nullptr, nullptr);

    const auto checkOne = [&world](const RenderView& view, Entity entity, PrimitiveId primitive, Vec3 color) {
        const std::vector<MeshInstance> found = instancesColored(view, color);
        REQUIRE(found.size() == 1);
        const MeshInstance& got = found[0];
        const Mat4 model = engine::worldMatrix(world, entity);
        const Mat4 viewProj = view.camera.proj * view.camera.view;
        CHECK((got.primitive == primitive));
        CHECK_FALSE(got.mesh.valid());
        CHECK(got.submesh == 0U);
        CHECK(got.palette.empty());
        CHECK(got.model == model);
        CHECK(got.normalMatrix == Mat4::identity());
        CHECK(got.mvp == viewProj * model);
        CHECK(got.color == color);
        CHECK((got.material == MaterialHandle{}));
    };
    for (const RenderView* view : {&withTable, &withoutTable}) {
        REQUIRE(view->hasCamera);
        REQUIRE(view->instances.size() == 3);
        CHECK(view->unresolvedMaterials == 0);
        CHECK(view->unresolvedMeshes == 0);
        checkOne(*view, cube, PrimitiveId::Cube, red);
        checkOne(*view, sphere, PrimitiveId::Sphere, green);
        checkOne(*view, clamped, PrimitiveId::Cube, blue);
    }
    for (const Vec3 color : {red, green, blue}) {
        const std::vector<MeshInstance> fromTable = instancesColored(withTable, color);
        const std::vector<MeshInstance> fromNull = instancesColored(withoutTable, color);
        REQUIRE(fromTable.size() == 1);
        REQUIRE(fromNull.size() == 1);
        CHECK(sameInstance(fromTable[0], fromNull[0]));
    }
    // ANTI-VACUITY: the model really carries the translation, so `model == worldMatrix` is not two
    // identities agreeing.
    CHECK(engine::worldMatrix(world, cube).columns[3].x == -1.0F);
}

TEST_CASE("scene_render bindings: a primitive's material count is PER EMITTED INSTANCE (BR27)") {
    World world;
    addCamera(world);
    const MeshRenderer sharing{.primitive = 0, .color = Vec3::one(), .material = guidOf(53)};
    for (int i = 0; i < 3; ++i) {
        const Transform at{Vec3{static_cast<float>(i), 0.0F, 0.0F}, Quat::identity(), Vec3::one()};
        addMeshEntity(world, at, sharing);
    }
    // An ARM-2 entity: a VALID mesh nothing resolves, carrying the SAME unbound material. It emits
    // NOTHING, so it counts one mesh and NO material (D5). Hoisting the resolve above the mesh branch
    // would make this 4.
    const MeshRenderer arm2{.primitive = 0, .color = Vec3::one(), .mesh = guidOf(2), .material = guidOf(53)};
    addMeshEntity(world, Transform{}, arm2);

    AssetBindingTable table;
    table.setMaterial(guidOf(60), MaterialHandle{3, 1});  // present, and names neither guid above

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    REQUIRE(view.instances.size() == 3);
    CHECK(view.unresolvedMaterials == 3);  // N primitives on one unbound guid count N -- never 1, never 4
    CHECK(view.unresolvedMeshes == 1);
}

TEST_CASE("scene_render bindings: the 0-camera early return keeps a primitive's handle and count (BR28)") {
    World world;  // NO camera: BR17's shape, for the material arm -- the instance walk precedes the camera
    const Vec3 boundColor{1.0F, 0.0F, 0.0F};
    const Vec3 unboundColor{0.0F, 1.0F, 0.0F};
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = boundColor, .material = guidOf(50)});
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 1, .color = unboundColor, .material = guidOf(54)});

    AssetBindingTable table;
    table.setMaterial(guidOf(50), MaterialHandle{7, 1});

    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, &table);
    CHECK_FALSE(view.hasCamera);
    CHECK(view.cameraCount == 0);
    REQUIRE(view.instances.size() == 2);
    const std::vector<MeshInstance> bound = instancesColored(view, boundColor);
    const std::vector<MeshInstance> unbound = instancesColored(view, unboundColor);
    REQUIRE(bound.size() == 1);
    REQUIRE(unbound.size() == 1);
    CHECK((bound[0].material == MaterialHandle{7, 1}));
    CHECK_FALSE(unbound[0].material.valid());
    CHECK(view.unresolvedMaterials == 1);
}

TEST_CASE("scene_render bindings: a MeshRenderer with no Transform counts no material either (BR29)") {
    World world;
    addCamera(world);
    const Entity orphan = world.create();
    const MeshRenderer orphanRenderer{.primitive = 0, .color = Vec3{0.4F, 0.4F, 0.4F}, .material = guidOf(55)};
    REQUIRE(world.add<MeshRenderer>(orphan, orphanRenderer) != nullptr);
    addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 1, .color = Vec3::one()});  // not an empty walk

    // A NULL table on purpose: if the orphan were visited at all, D4 would count its VALID material.
    RenderViewScratch scratch;
    const RenderView view = buildRenderView(world, scratch, VIEWPORT, nullptr, nullptr);
    REQUIRE(view.instances.size() == 1);
    CHECK((view.instances[0].primitive == PrimitiveId::Sphere));  // the ordinary one, never the orphan
    CHECK(view.unresolvedMaterials == 0);                         // each<Transform, MeshRenderer> skips it
}

// ================================================================================================
// Tier 1 -- a real Device, no window. Compiled only where the shader toolchain built the artifacts
// ForwardRenderer::create loads.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/assets/cooked_mesh.hpp>
    #include <aero/assets/mesh_cook.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    #include <array>
    #include <memory>
    #include <optional>
    #include <span>
    #include <utility>

namespace {

constexpr std::array<Vec3, 3> BR_POSITIONS{Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F}, Vec3{0.0F, 0.5F, 0.0F}};
constexpr std::array<Vec3, 3> BR_NORMALS{Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, 1.0F}};
constexpr std::array<engine::Vec2, 3> BR_UV0{engine::Vec2{0.0F, 0.0F}, engine::Vec2{1.0F, 0.0F},
                                             engine::Vec2{0.5F, 1.0F}};
constexpr std::array<std::uint32_t, 3> BR_INDICES{0, 1, 2};

// A TWO-MESH model, cooked in memory: both primitives share one attribute mask, so the cook groups
// them into one section with two submeshes carrying sourceMeshIndex 0 and 1 -- which is exactly the
// shape the emission arm's join key exists to separate. The returned bytes must OUTLIVE every
// CookedMesh parsed from them (the docs/09 section 9 span contract).
[[nodiscard]] std::vector<std::byte> cookTwoMeshModel() {
    engine::assets::MeshCookPrimitive first;
    first.sourceMeshIndex = 0;
    first.sourcePrimitiveIndex = 0;
    first.positions = BR_POSITIONS;
    first.normals = BR_NORMALS;
    first.uv0 = BR_UV0;
    first.indices = BR_INDICES;
    engine::assets::MeshCookPrimitive second = first;
    second.sourceMeshIndex = 1;
    second.sourcePrimitiveIndex = 0;
    const std::array<engine::assets::MeshCookPrimitive, 2> primitives{first, second};

    engine::assets::MeshCookResult result = engine::assets::cookMesh({.sourceGuid = {}, .primitives = primitives});
    REQUIRE(result.status == engine::assets::MeshCookStatus::Ok);
    REQUIRE_FALSE(result.bytes.empty());
    return std::move(result.bytes);
}

}  // namespace

TEST_CASE("scene_render bindings: draw() ignores the two unresolved counts (BR21)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    auto forward = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const Vec3 eye{0.0F, 0.0F, 3.0F};
    const engine::render::CameraView camera{engine::lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
                                            engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F), eye};

    // pipelineBindCount counts pipeline TRANSITIONS, not draws, and draw()'s reset bind is deliberately
    // uncounted -- so a one-instance view reads 0 whatever happens and could not tell "drew" from "did
    // not". Three instances ALTERNATING cull state (default -> doubleSided -> default) read exactly 2,
    // which is a number the two counts have to leave alone.
    const MaterialHandle doubleSided = forward->createMaterial({.doubleSided = true}, {});
    REQUIRE(doubleSided.valid());
    const auto instanceWith = [&](MaterialHandle material) {
        MeshInstance instance;
        instance.primitive = PrimitiveId::Cube;
        instance.model = Mat4::identity();
        instance.normalMatrix = Mat4::identity();
        instance.mvp = camera.proj * camera.view;
        instance.material = material;
        return instance;
    };
    const std::array<MeshInstance, 3> instances{instanceWith(MaterialHandle{}), instanceWith(doubleSided),
                                                instanceWith(MaterialHandle{})};

    const auto drawOnce = [&](std::uint32_t unresolvedMeshes, std::uint32_t unresolvedMaterials) -> std::size_t {
        RenderView view;
        view.camera = camera;
        view.directional = {.direction = Vec3{-0.5F, -1.0F, -0.3F}, .color = Vec3::one(), .intensity = 2.0F};
        view.instances = instances;
        view.unresolvedMeshes = unresolvedMeshes;
        view.unresolvedMaterials = unresolvedMaterials;

        const std::size_t before = forward->pipelineBindCount();
        std::optional<engine::render::Frame> open = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
        REQUIRE(open.has_value());
        forward->draw(*open, view);
        CHECK(target->endFrame(std::move(*open)));
        return forward->pipelineBindCount() - before;
    };

    const std::size_t withZero = drawOnce(0, 0);
    const std::size_t withSeven = drawOnce(7, 7);
    CHECK(withZero == 2);  // the recording really happened, so the equality below is not two zeroes
    CHECK(withSeven == withZero);
}

TEST_CASE("scene_render bindings: a cooked mesh resolves end to end through SceneRenderer (BR22)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
    REQUIRE(sceneRenderer.has_value());

    const std::vector<std::byte> bytes = cookTwoMeshModel();
    const engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    REQUIRE(parse.mesh.submeshes.size() == 2);

    // The handles are minted on THE RENDERER THAT DRAWS THEM -- which is the whole reason renderer()
    // exists, since a MeshHandle is per-ForwardRenderer.
    const MeshHandle mesh = sceneRenderer->renderer().createMesh(parse.mesh);
    REQUIRE(mesh.valid());
    REQUIRE(sceneRenderer->renderer().meshSubmeshCount(mesh) == 2);

    MeshBinding binding;
    binding.mesh = mesh;
    for (std::size_t i = 0; i < parse.mesh.submeshes.size(); ++i) {
        MeshBindingSubmesh sub;
        sub.submesh = static_cast<std::uint32_t>(i);
        sub.sourceMeshIndex = parse.mesh.submeshes[i].sourceMeshIndex;
        binding.submeshes.push_back(sub);
    }
    REQUIRE(binding.submeshes[0].sourceMeshIndex == 0U);
    REQUIRE(binding.submeshes[1].sourceMeshIndex == 1U);
    sceneRenderer->bindings().setMesh(guidOf(1), binding);
    CHECK(sceneRenderer->bindings().meshCount() == 1);

    // A doubleSided override material, bound by GUID: it is what makes the draw OBSERVABLE at all.
    // pipelineBindCount counts pipeline TRANSITIONS and draw()'s reset bind is uncounted, so a
    // single-instance view reads 0 unless the instance wants a pipeline other than the reset one --
    // and "which pipeline" is a function of the RESOLVED material, which is exactly the seam under
    // test. A material override that never reached the GPU reads 0 here.
    const MaterialHandle doubleSided = sceneRenderer->renderer().createMaterial({.doubleSided = true}, {});
    REQUIRE(doubleSided.valid());
    sceneRenderer->bindings().setMaterial(guidOf(50), doubleSided);
    CHECK(sceneRenderer->bindings().materialCount() == 1);

    World world;
    const Entity camEntity = world.create();
    REQUIRE(world.add<Transform>(camEntity, Transform{Vec3{0.0F, 0.0F, 3.0F}, Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<Camera>(camEntity, Camera{}) != nullptr);
    const Entity light = world.create();
    REQUIRE(world.add<Transform>(light) != nullptr);
    REQUIRE(world.add<DirectionalLight>(light, DirectionalLight{Vec3::one(), 1.0F}) != nullptr);
    const Entity drawn = addMeshEntity(world, Transform{},
                                       MeshRenderer{.primitive = 0,
                                                    .color = Vec3{0.8F, 0.7F, 0.6F},
                                                    .mesh = guidOf(1),
                                                    .meshIndex = 0,
                                                    .material = guidOf(50)});

    {
        std::optional<engine::render::Frame> open = target->beginFrame({0.05F, 0.05F, 0.08F, 1.0F});
        REQUIRE(open.has_value());
        const std::size_t before = sceneRenderer->renderer().pipelineBindCount();
        sceneRenderer->render(world, *open);
        // Submission is the assertion: a bound pipeline inconsistent with the recorded draw is what a
        // backend validation error looks like from here.
        CHECK(target->endFrame(std::move(*open)));
        CHECK(sceneRenderer->renderer().pipelineBindCount() - before == 1);
    }
    CHECK(sceneRenderer->lastUnresolvedMeshes() == 0);
    CHECK(sceneRenderer->lastUnresolvedMaterials() == 0);

    // A STALE meshIndex on the same binding: the latch must MOVE, or "== 0" above proves only that a
    // never-written member is zero-initialised.
    auto* const stale = world.get<MeshRenderer>(drawn);
    REQUIRE(stale != nullptr);
    stale->meshIndex = 7;
    {
        std::optional<engine::render::Frame> open = target->beginFrame({0.05F, 0.05F, 0.08F, 1.0F});
        REQUIRE(open.has_value());
        sceneRenderer->render(world, *open);
        CHECK(target->endFrame(std::move(*open)));
    }
    CHECK(sceneRenderer->lastUnresolvedMeshes() == 1);
    CHECK(sceneRenderer->lastUnresolvedMaterials() == 0);
}

TEST_CASE("scene_render bindings: a primitive's bound material reaches the draw through SceneRenderer (BR30)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
    REQUIRE(sceneRenderer.has_value());

    // BR22's argument, one arm over: a doubleSided material is what makes the draw OBSERVABLE. Minted on
    // THE RENDERER THAT DRAWS IT, and bound by GUID exactly as the editor's ledger binds one. It starts from
    // DEFAULT_MATERIAL_PARAMS (metallic 0), the fixture rule, although only its two-sidedness is read here.
    engine::render::MaterialParams twoSidedParams = engine::render::DEFAULT_MATERIAL_PARAMS;
    twoSidedParams.doubleSided = true;
    const MaterialHandle doubleSided = sceneRenderer->renderer().createMaterial(twoSidedParams, {});
    REQUIRE(doubleSided.valid());
    sceneRenderer->bindings().setMaterial(guidOf(50), doubleSided);
    REQUIRE(sceneRenderer->bindings().materialCount() == 1);

    World world;
    const Entity camEntity = world.create();
    const Transform camTransform{Vec3{0.0F, 0.0F, 3.0F}, Quat::identity(), Vec3::one()};
    REQUIRE(world.add<Transform>(camEntity, camTransform) != nullptr);
    REQUIRE(world.add<Camera>(camEntity, Camera{}) != nullptr);
    const Entity light = world.create();
    REQUIRE(world.add<Transform>(light) != nullptr);
    REQUIRE(world.add<DirectionalLight>(light, DirectionalLight{Vec3::one(), 1.0F}) != nullptr);
    // ONE Cube PRIMITIVE -- `mesh` NIL -- where BR22 has a cooked mesh.
    const Entity cube = addMeshEntity(world, Transform{}, MeshRenderer{.primitive = 0, .color = Vec3::one()});
    auto* const cubeRenderer = world.get<MeshRenderer>(cube);
    REQUIRE(cubeRenderer != nullptr);
    REQUIRE_FALSE(cubeRenderer->mesh.valid());

    // One frame through the WHOLE SceneRenderer::render (shadow + sky + forward), returning the
    // pipeline-transition delta it produced.
    const auto renderOnce = [&]() -> std::size_t {
        std::optional<engine::render::Frame> open = target->beginFrame({0.05F, 0.05F, 0.08F, 1.0F});
        REQUIRE(open.has_value());
        const std::size_t before = sceneRenderer->renderer().pipelineBindCount();
        sceneRenderer->render(world, *open);
        CHECK(target->endFrame(std::move(*open)));
        return sceneRenderer->renderer().pipelineBindCount() - before;
    };

    // Frame 1 -- NIL: the default material, the reset pipeline, no transition, nothing counted.
    const std::size_t nilDelta = renderOnce();  // outside CHECK: a REQUIRE inside must abort the case
    CHECK(nilDelta == 0U);
    CHECK(sceneRenderer->lastUnresolvedMaterials() == 0U);
    CHECK(sceneRenderer->renderer().lastFrameDrawn() == 1U);  // ANTI-VACUITY: the primitive really drew

    // Frame 2 -- BOUND: the doubleSided material reached the draw, so the primitive arm bound the
    // cull-none pipeline. This 1 is the number E.5.1's defect makes 0.
    cubeRenderer->material = guidOf(50);
    const std::size_t boundDelta = renderOnce();
    CHECK(boundDelta == 1U);
    CHECK(sceneRenderer->lastUnresolvedMaterials() == 0U);
    CHECK(sceneRenderer->renderer().lastFrameDrawn() == 1U);

    // Frame 3 -- UNBOUND: the default again, and the latch MOVES, so the two zeroes above are not a
    // never-written member.
    cubeRenderer->material = guidOf(51);
    const std::size_t unboundDelta = renderOnce();
    CHECK(unboundDelta == 0U);
    CHECK(sceneRenderer->lastUnresolvedMaterials() == 1U);
    CHECK(sceneRenderer->renderer().lastFrameDrawn() == 1U);
}

#endif  // AERO_SHADER_TOOLS_ENABLED

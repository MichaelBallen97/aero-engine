// tests/scene_render_selection_test.cpp — task E.1.4: buildSelectionMaskSet, the PURE World ->
// selection-mask bridge (SQ1-SQ12). NO GPU, NO shader toolchain and therefore NO #if OF ANY KIND in
// this file: every case here runs in every configuration, including both reduced ones.
//
// The builder resolves each selected entity through the SAME three arms buildRenderView uses, which
// is why it lives beside it in scene_renderer.cpp -- and why SQ2 compares the two field by field
// rather than restating what an arm ought to produce.

#include <aero/core/math.hpp>
#include <aero/render/render.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>  // MSVC: any CHECK on a string_view needs this (the 0.4.1 trap)
#include <span>
#include <vector>

using engine::Entity;
using engine::Guid;
using engine::Mat4;
using engine::MeshRenderer;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::render::MeshInstance;
using engine::render::PrimitiveId;
using engine::scene_render::AssetBindingTable;
using engine::scene_render::buildRenderView;
using engine::scene_render::buildSelectionMaskSet;
using engine::scene_render::MeshBinding;
using engine::scene_render::MeshBindingSubmesh;
using engine::scene_render::RenderViewScratch;
using engine::scene_render::SelectionMaskScratch;
using engine::scene_render::SelectionMaskSet;

namespace {

constexpr engine::rhi::Extent2D VIEWPORT{1920U, 1080U};

// A camera the builder is HANDED rather than one it finds: buildSelectionMaskSet takes the view the
// caller is about to render, by value semantics, and never walks the World for one.
[[nodiscard]] engine::render::CameraView testCamera() {
    engine::render::CameraView camera;
    camera.view = engine::lookAt(Vec3{3.0F, 4.0F, 5.0F}, Vec3{0.0F, 0.5F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    camera.proj = engine::perspective(1.0F, 16.0F / 9.0F, 0.1F, 100.0F);
    camera.eyePosition = Vec3{3.0F, 4.0F, 5.0F};
    return camera;
}

// An entity that draws a built-in primitive, at a DISTINCT transform so a swapped pair cannot pass.
[[nodiscard]] Entity makePrimitiveEntity(World& world, Vec3 position, std::uint32_t primitive, Vec3 color) {
    const Entity entity = world.create();
    REQUIRE(world.add<Transform>(entity, Transform{position, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(world.add<MeshRenderer>(entity, MeshRenderer{.primitive = primitive, .color = color}) != nullptr);
    return entity;
}

[[nodiscard]] Entity makeReferencingEntity(World& world, Vec3 position, Guid mesh, std::uint32_t meshIndex,
                                           Guid material = {}) {
    const Entity entity = world.create();
    REQUIRE(world.add<Transform>(entity, Transform{position, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(world.add<MeshRenderer>(entity, MeshRenderer{.mesh = mesh, .meshIndex = meshIndex, .material = material}) !=
            nullptr);
    return entity;
}

[[nodiscard]] Guid guidFrom(std::uint64_t high, std::uint64_t low) { return Guid{high, low}; }

// Field-by-field equality of the two builders' instances, so a failure names WHICH field moved rather
// than reporting that two aggregates differ.
void checkInstancesEqual(const MeshInstance& actual, const MeshInstance& expected) {
    CHECK((actual.primitive == expected.primitive));
    CHECK((actual.mesh == expected.mesh));
    CHECK(actual.submesh == expected.submesh);
    CHECK((actual.material == expected.material));
    CHECK(actual.color == expected.color);
    CHECK(actual.model == expected.model);
    CHECK(actual.normalMatrix == expected.normalMatrix);
    CHECK(actual.mvp == expected.mvp);
    CHECK(actual.palette.empty() == expected.palette.empty());
}

// MATCHED BY (model, submesh), NEVER BY POSITION -- and that is a finding rather than a nicety.
// buildRenderView walks the World through each<Transform, MeshRenderer>, whose order is EnTT's
// storage order, while buildSelectionMaskSet walks the SELECTION. A positional comparison would be
// asserting that two unrelated orders happen to agree, which they do not. Each fixture below gives
// every entity a distinct translation, so the pair is a unique key -- and REQUIRE(matches == 1) is
// what makes that a checked property rather than an assumption.
void checkMatchedAgainst(std::span<const MeshInstance> reference, const MeshInstance& actual) {
    const MeshInstance* found = nullptr;
    int matches = 0;
    for (const MeshInstance& candidate : reference) {
        if (candidate.model == actual.model && candidate.submesh == actual.submesh) {
            found = &candidate;
            ++matches;
        }
    }
    REQUIRE(matches == 1);
    checkInstancesEqual(actual, *found);
}

}  // namespace

TEST_CASE("scene_render selection: an EMPTY selection produces nothing and allocates nothing (SQ1)") {
    World world;
    SelectionMaskScratch scratch;
    // Warm the scratch first, so "does not touch the capacity" is a claim about a REUSED scratch
    // rather than about an empty one that never had any.
    scratch.secondary.reserve(8);
    scratch.primary.reserve(8);
    scratch.withoutGeometry.reserve(8);
    const std::size_t secondaryCapacity = scratch.secondary.capacity();
    const std::size_t primaryCapacity = scratch.primary.capacity();
    const std::size_t markerCapacity = scratch.withoutGeometry.capacity();

    const SelectionMaskSet set = buildSelectionMaskSet(world, {}, Entity{}, testCamera(), scratch);
    CHECK(set.secondary.empty());
    CHECK(set.primary.empty());
    CHECK(set.withoutGeometry.empty());
    CHECK(set.skippedOverCap == 0U);
    CHECK(set.unresolvedMeshes == 0U);
    CHECK(set.unresolvedMaterials == 0U);
    CHECK(scratch.secondary.capacity() == secondaryCapacity);
    CHECK(scratch.primary.capacity() == primaryCapacity);
    CHECK(scratch.withoutGeometry.capacity() == markerCapacity);
}

TEST_CASE("scene_render selection: the three arms match buildRenderView's, field for field (SQ2)") {
    World world;
    AssetBindingTable bindings;
    const Guid resolvedMesh = guidFrom(1, 2);
    const Guid missingMesh = guidFrom(3, 4);
    // A binding with THREE submeshes over TWO source meshes, so the sourceMeshIndex filter is
    // exercised rather than trivially satisfied.
    MeshBinding binding;
    binding.mesh = engine::render::MeshHandle{7U, 1U};
    binding.submeshes = {MeshBindingSubmesh{.submesh = 0U, .sourceMeshIndex = 0U},
                         MeshBindingSubmesh{.submesh = 1U, .sourceMeshIndex = 1U},
                         MeshBindingSubmesh{.submesh = 2U, .sourceMeshIndex = 0U}};
    bindings.setMesh(resolvedMesh, binding);

    const Entity primitiveEntity = makePrimitiveEntity(world, Vec3{1.0F, 0.0F, 0.0F}, 2U, Vec3{0.25F, 0.5F, 0.75F});
    const Entity resolvedEntity = makeReferencingEntity(world, Vec3{0.0F, 2.0F, 0.0F}, resolvedMesh, 0U);
    const Entity unresolvedEntity = makeReferencingEntity(world, Vec3{0.0F, 0.0F, 3.0F}, missingMesh, 0U);

    const engine::render::CameraView camera = testCamera();
    RenderViewScratch viewScratch;
    const engine::render::RenderView view = buildRenderView(world, viewScratch, VIEWPORT, &camera, &bindings);
    SelectionMaskScratch scratch;
    const std::array<Entity, 3> selected{primitiveEntity, resolvedEntity, unresolvedEntity};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, camera, scratch, &bindings);

    // buildRenderView emits: 1 primitive + 2 matching submeshes = 3; the unresolved one emits none.
    REQUIRE(view.instances.size() == 3U);
    REQUIRE(set.secondary.size() == 3U);
    CHECK(set.primary.empty());
    for (std::size_t i = 0; i < set.secondary.size(); ++i) {
        INFO("mask instance ", i);
        checkMatchedAgainst(view.instances, set.secondary[i]);
    }
    // The unresolved reference lands in the MARKER list on both counts.
    REQUIRE(set.withoutGeometry.size() == 1U);
    CHECK(set.withoutGeometry[0] == unresolvedEntity);
    CHECK(set.unresolvedMeshes == 1U);
    CHECK(view.unresolvedMeshes == 1U);
}

TEST_CASE("scene_render selection: a MeshRenderer with NO Transform is a MARKER, not an outline (SQ3)") {
    // NOT DEFENSIVE: buildRenderView walks each<Transform, MeshRenderer>, so such an entity is NOT
    // DRAWN by the forward pass at all -- an outline for it could never appear, and the marker is the
    // only honest answer.
    World world;
    const Entity transformless = world.create();
    REQUIRE(world.add<MeshRenderer>(transformless, MeshRenderer{}) != nullptr);
    const Entity drawn = makePrimitiveEntity(world, Vec3{1.0F, 1.0F, 1.0F}, 0U, Vec3::one());

    const engine::render::CameraView camera = testCamera();
    RenderViewScratch viewScratch;
    const engine::render::RenderView view = buildRenderView(world, viewScratch, VIEWPORT, &camera);
    CHECK(view.instances.size() == 1U);  // the forward pass draws ONE of the two

    SelectionMaskScratch scratch;
    const std::array<Entity, 2> selected{transformless, drawn};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, camera, scratch);
    CHECK(set.secondary.size() == 1U);
    REQUIRE(set.withoutGeometry.size() == 1U);
    CHECK(set.withoutGeometry[0] == transformless);

    SUBCASE("...and so is an entity with a Transform but NO MeshRenderer") {
        const Entity bare = world.create();
        REQUIRE(world.add<Transform>(bare, Transform{}) != nullptr);
        const std::array<Entity, 1> bareOnly{bare};
        const SelectionMaskSet bareSet = buildSelectionMaskSet(world, bareOnly, Entity{}, camera, scratch);
        CHECK(bareSet.secondary.empty());
        CHECK(bareSet.primary.empty());
        REQUIRE(bareSet.withoutGeometry.size() == 1U);
        CHECK(bareSet.withoutGeometry[0] == bare);
    }
}

TEST_CASE("scene_render selection: EXACTLY the primary entity's instances land in `primary` (SQ4)") {
    World world;
    AssetBindingTable bindings;
    const Guid seven = guidFrom(9, 9);
    MeshBinding binding;
    binding.mesh = engine::render::MeshHandle{3U, 1U};
    for (std::uint32_t i = 0; i < 7U; ++i) {
        binding.submeshes.push_back(MeshBindingSubmesh{.submesh = i, .sourceMeshIndex = 0U});
    }
    bindings.setMesh(seven, binding);

    const Entity a = makePrimitiveEntity(world, Vec3{1.0F, 0.0F, 0.0F}, 0U, Vec3::one());
    const Entity sevenSubmeshes = makeReferencingEntity(world, Vec3{0.0F, 1.0F, 0.0F}, seven, 0U);
    const Entity c = makePrimitiveEntity(world, Vec3{0.0F, 0.0F, 1.0F}, 1U, Vec3::one());

    SelectionMaskScratch scratch;
    const std::array<Entity, 3> selected{a, sevenSubmeshes, c};
    const SelectionMaskSet set =
        buildSelectionMaskSet(world, selected, sevenSubmeshes, testCamera(), scratch, &bindings);
    // INSTANCES ARE NOT CAPPED: a seven-submesh primary contributes SEVEN, because the forward pass
    // has no instance cap either.
    CHECK(set.primary.size() == 7U);
    CHECK(set.secondary.size() == 2U);
    CHECK(set.withoutGeometry.empty());
    for (const MeshInstance& instance : set.primary) {
        CHECK((instance.mesh == binding.mesh));
    }
}

TEST_CASE("scene_render selection: a `primary` handle ABSENT from `selected` is not an error (SQ5)") {
    // Selection guarantees the pairing; this takes a span plus a handle rather than a const
    // Selection& so a tier-0 case can drive it from a plain array -- which means it must ALSO be
    // total over a pairing Selection would never produce.
    World world;
    const Entity a = makePrimitiveEntity(world, Vec3{1.0F, 0.0F, 0.0F}, 0U, Vec3::one());
    const Entity b = makePrimitiveEntity(world, Vec3{0.0F, 1.0F, 0.0F}, 0U, Vec3::one());
    const Entity elsewhere = makePrimitiveEntity(world, Vec3{0.0F, 0.0F, 1.0F}, 0U, Vec3::one());

    SelectionMaskScratch scratch;
    const std::array<Entity, 2> selected{a, b};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, elsewhere, testCamera(), scratch);
    CHECK(set.primary.empty());
    CHECK(set.secondary.size() == 2U);
    CHECK(set.skippedOverCap == 0U);

    SUBCASE("...and so is a NULL primary handle") {
        const SelectionMaskSet nullPrimary = buildSelectionMaskSet(world, selected, Entity{}, testCamera(), scratch);
        CHECK(nullPrimary.primary.empty());
        CHECK(nullPrimary.secondary.size() == 2U);
    }
}

TEST_CASE("scene_render selection: dead handles are SKIPPED and do not consume cap budget (SQ6)") {
    // 2.3.2's A7 rule, and it is what forces the dead-handle test ABOVE the cap test: a run of dead
    // handles at the front would otherwise exhaust the budget, and a dead handle past position 256
    // would be counted as over-cap.
    World world;
    std::vector<Entity> selected;
    for (int i = 0; i < 10; ++i) {
        const Entity dead = world.create();
        world.destroy(dead);
        selected.push_back(dead);  // 10 DEAD handles, all at the FRONT
    }
    for (int i = 0; i < 300; ++i) {
        selected.push_back(makePrimitiveEntity(world, Vec3{static_cast<float>(i), 0.0F, 0.0F}, 0U, Vec3::one()));
    }

    SelectionMaskScratch scratch;
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, testCamera(), scratch, nullptr, 256U);
    // 256 processed of the 300 live ones, 44 over cap -- the ten dead handles cost NOTHING.
    CHECK(set.secondary.size() == 256U);
    CHECK(set.skippedOverCap == 44U);
    CHECK(set.withoutGeometry.empty());

    SUBCASE("a NULL handle is skipped on the same terms") {
        std::vector<Entity> withNulls;
        withNulls.emplace_back();  // default-constructed: never alive
        withNulls.emplace_back();
        for (int i = 0; i < 3; ++i) {
            withNulls.push_back(makePrimitiveEntity(world, Vec3{0.0F, static_cast<float>(i), 0.0F}, 0U, Vec3::one()));
        }
        const SelectionMaskSet nullSet =
            buildSelectionMaskSet(world, withNulls, Entity{}, testCamera(), scratch, nullptr, 3U);
        CHECK(nullSet.secondary.size() == 3U);
        CHECK(nullSet.skippedOverCap == 0U);
    }
}

TEST_CASE("scene_render selection: the cap holds in SELECTION ORDER (SQ7)") {
    World world;
    std::vector<Entity> selected;
    selected.reserve(300);
    for (int i = 0; i < 300; ++i) {
        selected.push_back(makePrimitiveEntity(world, Vec3{static_cast<float>(i), 0.0F, 0.0F}, 0U, Vec3::one()));
    }

    SelectionMaskScratch scratch;
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, testCamera(), scratch, nullptr, 256U);
    REQUIRE(set.secondary.size() == 256U);
    CHECK(set.skippedOverCap == 44U);
    // THE IDENTITY of the first and the 256th, so "in selection order" is asserted rather than
    // implied by a count that any order would satisfy. Each entity's translation is its index, so its
    // model matrix names it.
    CHECK(set.secondary.front().model == engine::translation(Vec3{0.0F, 0.0F, 0.0F}));
    CHECK(set.secondary.back().model == engine::translation(Vec3{255.0F, 0.0F, 0.0F}));
}

TEST_CASE("scene_render selection: every mvp is proj * view * model, RECOMPUTED independently (SQ8)") {
    // THE CASE MOST AT RISK OF ASSERTING NOTHING. The expectation is recomputed from
    // worldMatrix(world, e) and the camera -- NEVER read back off the MeshInstance the builder wrote,
    // which is the shape E.1.2's sabotage pass found passing 13.3 million assertions on a plainly
    // broken product.
    World world;
    const Entity parent = world.create();
    REQUIRE(world.add<Transform>(parent,
                                 Transform{Vec3{5.0F, 0.0F, -2.0F}, engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, 0.7F),
                                           Vec3{2.0F, 2.0F, 2.0F}}) != nullptr);
    const Entity child = makePrimitiveEntity(world, Vec3{0.0F, 1.5F, 0.0F}, 1U, Vec3::one());
    REQUIRE(world.setParent(child, parent));
    const Entity loose = makePrimitiveEntity(world, Vec3{-3.0F, 1.0F, 4.0F}, 2U, Vec3::one());

    const engine::render::CameraView camera = testCamera();
    SelectionMaskScratch scratch;
    const std::array<Entity, 2> selected{child, loose};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, camera, scratch);
    REQUIRE(set.secondary.size() == 2U);

    const Mat4 viewProj = camera.proj * camera.view;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        INFO("entity index ", i);
        const Mat4 expectedModel = engine::worldMatrix(world, selected[i]);
        const Mat4 expectedMvp = viewProj * expectedModel;
        // EXACT ==, not approxEquals: it is the same two multiplications in the same order, so it is
        // bit-exact. A tolerance here would hide a transposed or mis-composed product.
        CHECK(set.secondary[i].model == expectedModel);
        CHECK(set.secondary[i].mvp == expectedMvp);
    }
    // ...and the two entities' matrices really DIFFER, so a builder that wrote one instance twice
    // could not satisfy the loop above.
    CHECK(set.secondary[0].mvp != set.secondary[1].mvp);
}

TEST_CASE("scene_render selection: `palette` is EMPTY on every emitted instance (SQ9)") {
    // A statement about the tree rather than an omission: nothing in scene_render or editor/ fills a
    // palette, so a skinned mesh draws in BIND POSE in the viewport -- and a bind-pose mask is
    // exactly the right mask for a bind-pose picture.
    World world;
    AssetBindingTable bindings;
    const Guid mesh = guidFrom(11, 12);
    MeshBinding binding;
    binding.mesh = engine::render::MeshHandle{5U, 1U};
    binding.submeshes = {MeshBindingSubmesh{.submesh = 0U, .sourceMeshIndex = 0U},
                         MeshBindingSubmesh{.submesh = 1U, .sourceMeshIndex = 0U}};
    bindings.setMesh(mesh, binding);

    const Entity referencing = makeReferencingEntity(world, Vec3{1.0F, 1.0F, 1.0F}, mesh, 0U);
    const Entity primitive = makePrimitiveEntity(world, Vec3{2.0F, 0.0F, 0.0F}, 0U, Vec3::one());

    SelectionMaskScratch scratch;
    const std::array<Entity, 2> selected{referencing, primitive};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, primitive, testCamera(), scratch, &bindings);
    REQUIRE(set.secondary.size() == 2U);
    REQUIRE(set.primary.size() == 1U);
    for (const MeshInstance& instance : set.secondary) {
        CHECK(instance.palette.empty());
    }
    for (const MeshInstance& instance : set.primary) {
        CHECK(instance.palette.empty());
    }
}

TEST_CASE("scene_render selection: a NULL bindings table markers every reference (SQ10)") {
    // The sample/runtime/test case, and NOT an error: a missing table is the ordinary in-flight state
    // between a drop and the ledger's upload. VP3's *unresolved* half moved here.
    World world;
    const Entity referencing = makeReferencingEntity(world, Vec3{1.0F, 0.0F, 0.0F}, guidFrom(1, 1), 0U);
    const Entity alsoReferencing = makeReferencingEntity(world, Vec3{0.0F, 1.0F, 0.0F}, guidFrom(2, 2), 3U);
    const Entity primitive = makePrimitiveEntity(world, Vec3{0.0F, 0.0F, 1.0F}, 1U, Vec3{0.5F, 0.5F, 0.5F});

    SelectionMaskScratch scratch;
    const std::array<Entity, 3> selected{referencing, alsoReferencing, primitive};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, testCamera(), scratch, nullptr);
    // The PRIMITIVE arm is unaffected by a null table -- it never consults one.
    REQUIRE(set.secondary.size() == 1U);
    CHECK((set.secondary[0].primitive == PrimitiveId::Sphere));
    CHECK(set.secondary[0].color == Vec3{0.5F, 0.5F, 0.5F});
    REQUIRE(set.withoutGeometry.size() == 2U);
    CHECK(set.withoutGeometry[0] == referencing);
    CHECK(set.withoutGeometry[1] == alsoReferencing);
    CHECK(set.unresolvedMeshes == 2U);

    SUBCASE("a table that resolves the MESH but matches ZERO submeshes markers it too") {
        AssetBindingTable bindings;
        MeshBinding binding;
        binding.mesh = engine::render::MeshHandle{4U, 1U};
        binding.submeshes = {MeshBindingSubmesh{.submesh = 0U, .sourceMeshIndex = 0U}};
        bindings.setMesh(guidFrom(2, 2), binding);  // alsoReferencing names meshIndex 3, which matches none
        const SelectionMaskSet stale =
            buildSelectionMaskSet(world, selected, Entity{}, testCamera(), scratch, &bindings);
        CHECK(stale.unresolvedMeshes == 2U);
        REQUIRE(stale.withoutGeometry.size() == 2U);
        CHECK(stale.withoutGeometry[1] == alsoReferencing);
    }
}

TEST_CASE("scene_render selection: the scratch is CLEARED on entry and reused when warm (SQ11)") {
    World world;
    const Entity a = makePrimitiveEntity(world, Vec3{1.0F, 0.0F, 0.0F}, 0U, Vec3::one());
    const Entity b = makePrimitiveEntity(world, Vec3{0.0F, 1.0F, 0.0F}, 1U, Vec3::one());
    const Entity c = makePrimitiveEntity(world, Vec3{0.0F, 0.0F, 1.0F}, 2U, Vec3::one());
    const Entity markerish = world.create();  // no Transform, no MeshRenderer -> the marker list

    SelectionMaskScratch scratch;
    const std::array<Entity, 3> first{a, b, markerish};
    const SelectionMaskSet firstSet = buildSelectionMaskSet(world, first, b, testCamera(), scratch);
    CHECK(firstSet.secondary.size() == 1U);
    CHECK(firstSet.primary.size() == 1U);
    CHECK(firstSet.withoutGeometry.size() == 1U);
    const std::size_t secondaryCapacity = scratch.secondary.capacity();
    const std::size_t primaryCapacity = scratch.primary.capacity();
    const std::size_t markerCapacity = scratch.withoutGeometry.capacity();

    // A DIFFERENT selection, no larger than the first: NO RESIDUE, and NO ALLOCATION.
    const std::array<Entity, 1> second{c};
    const SelectionMaskSet secondSet = buildSelectionMaskSet(world, second, Entity{}, testCamera(), scratch);
    CHECK(secondSet.secondary.size() == 1U);
    CHECK(secondSet.primary.empty());
    CHECK(secondSet.withoutGeometry.empty());
    CHECK((secondSet.secondary[0].primitive == PrimitiveId::Plane));  // c's primitive, not a's or b's
    CHECK(scratch.secondary.capacity() == secondaryCapacity);
    CHECK(scratch.primary.capacity() == primaryCapacity);
    CHECK(scratch.withoutGeometry.capacity() == markerCapacity);
    // ...and the counters reset with it, rather than accumulating across calls.
    CHECK(secondSet.unresolvedMeshes == 0U);
    CHECK(secondSet.skippedOverCap == 0U);
}

TEST_CASE("scene_render selection: buildRenderView is OBSERVATIONALLY UNCHANGED by the split (SQ12)") {
    // resolveMaterial's last parameter became the COUNTER rather than the RenderView. That is a
    // claim, and this case plus the untouched scene_render_bindings_test.cpp battery are what make it
    // one -- three arms, in the order that IS the specification.
    World world;
    AssetBindingTable bindings;
    const Guid mesh = guidFrom(21, 22);
    const Guid goodOverride = guidFrom(31, 32);
    const Guid badOverride = guidFrom(41, 42);
    const engine::render::MaterialHandle overrideHandle{11U, 1U};
    const engine::render::MaterialHandle submeshOwn{22U, 1U};
    bindings.setMaterial(goodOverride, overrideHandle);

    MeshBinding binding;
    binding.mesh = engine::render::MeshHandle{2U, 1U};
    binding.submeshes = {MeshBindingSubmesh{.submesh = 0U, .sourceMeshIndex = 0U, .material = submeshOwn},
                         MeshBindingSubmesh{.submesh = 1U, .sourceMeshIndex = 0U, .material = submeshOwn},
                         MeshBindingSubmesh{.submesh = 2U, .sourceMeshIndex = 0U, .material = submeshOwn}};
    bindings.setMesh(mesh, binding);

    // Arm 1: an entity-level override that RESOLVES wins on EVERY submesh.
    const Entity resolves = makeReferencingEntity(world, Vec3{1.0F, 0.0F, 0.0F}, mesh, 0U, goodOverride);
    // Arm 2: an override that does NOT resolve is COUNTED and falls through -- it neither silently
    // becomes the submesh's own material without a trace nor blanks the draw.
    const Entity doesNot = makeReferencingEntity(world, Vec3{0.0F, 1.0F, 0.0F}, mesh, 0U, badOverride);
    // Arm 3: no override at all -> the submesh's own bound handle.
    const Entity noOverride = makeReferencingEntity(world, Vec3{0.0F, 0.0F, 1.0F}, mesh, 0U);

    const engine::render::CameraView camera = testCamera();
    RenderViewScratch viewScratch;
    const engine::render::RenderView view = buildRenderView(world, viewScratch, VIEWPORT, &camera, &bindings);
    REQUIRE(view.instances.size() == 9U);  // three entities x three matching submeshes
    // THE COUNT FIRES ONCE PER EMITTED SUBMESH, not once per entity: it answers "how many DRAWS could
    // not use the material they were asked for", and an entity-level count would understate a
    // three-submesh model by a factor of three.
    CHECK(view.unresolvedMaterials == 3U);
    // BY ENTITY, never by index: buildRenderView walks each<Transform, MeshRenderer> in EnTT's
    // storage order, so the three entities' instance runs are not where a reader expects them.
    const auto checkEntityMaterial = [&](Entity entity, engine::render::MaterialHandle expected) {
        const Mat4 model = engine::worldMatrix(world, entity);
        int seen = 0;
        for (const MeshInstance& instance : view.instances) {
            if (instance.model == model) {
                ++seen;
                CHECK((instance.material == expected));
            }
        }
        CHECK(seen == 3);  // all three submeshes, and the loop really ran
    };
    checkEntityMaterial(resolves, overrideHandle);  // arm 1: an override that RESOLVES wins
    checkEntityMaterial(doesNot, submeshOwn);       // arm 2: falls through, and is COUNTED
    checkEntityMaterial(noOverride, submeshOwn);    // arm 3: the submesh's own bound handle

    // ...and the mask builder reaches the identical three decisions and the identical count.
    SelectionMaskScratch scratch;
    const std::array<Entity, 3> selected{resolves, doesNot, noOverride};
    const SelectionMaskSet set = buildSelectionMaskSet(world, selected, Entity{}, camera, scratch, &bindings);
    REQUIRE(set.secondary.size() == 9U);
    CHECK(set.unresolvedMaterials == 3U);
    for (std::size_t i = 0; i < set.secondary.size(); ++i) {
        INFO("mask instance ", i);
        checkMatchedAgainst(view.instances, set.secondary[i]);
    }
}

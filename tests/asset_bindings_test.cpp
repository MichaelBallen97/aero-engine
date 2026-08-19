// tests/asset_bindings_test.cpp -- task 3.1.5: engine::scene_render::AssetBindingTable, the GUID ->
// renderer-handle table (AB1-AB14). Tier-0: no GPU, no World, no ImGui -- the table is pure storage,
// and its whole contract is "sorted vector + lower_bound" behaving like a map that refuses nil keys.

#include <aero/core/guid.hpp>
#include <aero/render/render.hpp>
#include <aero/scene_render/asset_bindings.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>  // MSVC-only: doctest's DOCTEST_STRINGIFY on a string_view needs it (0.4.1 trap)
#include <type_traits>
#include <vector>

using engine::Guid;
using engine::render::MaterialHandle;
using engine::render::MeshHandle;
using engine::scene_render::AssetBindingTable;
using engine::scene_render::MeshBinding;
using engine::scene_render::MeshBindingSubmesh;

namespace {

// Deterministic, hand-written guids: `hi` carries the ordinal so `guidOf(a) < guidOf(b)` iff a < b
// under Guid::operator<'s (hi, then lo) order -- which is what makes the insertion-order cases below
// assert something about the SORT rather than about a generator.
[[nodiscard]] Guid guidOf(std::uint64_t ordinal) { return Guid{ordinal, 0x5150ULL}; }

[[nodiscard]] MeshHandle meshHandleOf(std::uint32_t index) { return MeshHandle{index, 1U}; }

[[nodiscard]] MaterialHandle materialHandleOf(std::uint32_t index) { return MaterialHandle{index, 1U}; }

[[nodiscard]] MeshBinding bindingOf(std::uint32_t meshIndex, std::size_t submeshCount) {
    MeshBinding binding;
    binding.mesh = meshHandleOf(meshIndex);
    for (std::size_t i = 0; i < submeshCount; ++i) {
        binding.submeshes.push_back(MeshBindingSubmesh{static_cast<std::uint32_t>(i), 0U, MaterialHandle{}});
    }
    return binding;
}

}  // namespace

TEST_CASE("scene_render: setMesh/findMesh round trip (AB1)") {
    AssetBindingTable table;
    table.setMesh(guidOf(7), bindingOf(42, 3));

    const MeshBinding* found = table.findMesh(guidOf(7));
    REQUIRE(found != nullptr);
    CHECK(found->mesh == meshHandleOf(42));
    CHECK(found->submeshes.size() == 3);
    CHECK(found->submeshes[2].submesh == 2U);
    CHECK(table.meshCount() == 1);
}

TEST_CASE("scene_render: an empty table finds nothing (AB2)") {
    const AssetBindingTable table;
    CHECK(table.findMesh(guidOf(1)) == nullptr);
    CHECK_FALSE(table.findMaterial(guidOf(1)).valid());
    CHECK(table.meshCount() == 0);
    CHECK(table.materialCount() == 0);
}

TEST_CASE("scene_render: a nil guid is refused as a key (AB3)") {
    AssetBindingTable table;
    table.setMesh(Guid{}, bindingOf(1, 1));
    table.setMaterial(Guid{}, materialHandleOf(1));
    CHECK(table.meshCount() == 0);
    CHECK(table.materialCount() == 0);
}

TEST_CASE("scene_render: a nil guid finds nothing even beside real entries (AB4)") {
    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(1, 1));
    table.setMaterial(guidOf(1), materialHandleOf(9));

    CHECK(table.findMesh(Guid{}) == nullptr);
    CHECK_FALSE(table.findMaterial(Guid{}).valid());
    // and the real entries are untouched by the nil probes
    CHECK(table.meshCount() == 1);
    CHECK(table.materialCount() == 1);
}

TEST_CASE("scene_render: setMesh on an existing guid REPLACES, never duplicates (AB5)") {
    AssetBindingTable table;
    table.setMesh(guidOf(4), bindingOf(10, 2));
    table.setMesh(guidOf(4), bindingOf(11, 5));

    CHECK(table.meshCount() == 1);
    const MeshBinding* found = table.findMesh(guidOf(4));
    REQUIRE(found != nullptr);
    CHECK(found->mesh == meshHandleOf(11));
    CHECK(found->submeshes.size() == 5);
}

TEST_CASE("scene_render: descending insertion still leaves the vector sorted (AB6)") {
    AssetBindingTable table;
    for (std::uint64_t ordinal = 9; ordinal >= 1; --ordinal) {
        table.setMesh(guidOf(ordinal), bindingOf(static_cast<std::uint32_t>(ordinal), 1));
    }
    REQUIRE(table.meshCount() == 9);
    // Every one of them is findable -- which a lower_bound over an UNSORTED vector cannot deliver.
    for (std::uint64_t ordinal = 1; ordinal <= 9; ++ordinal) {
        const MeshBinding* found = table.findMesh(guidOf(ordinal));
        REQUIRE(found != nullptr);
        CHECK(found->mesh == meshHandleOf(static_cast<std::uint32_t>(ordinal)));
    }
}

TEST_CASE("scene_render: removeMesh takes the first, middle and last entries (AB7)") {
    AssetBindingTable table;
    for (std::uint64_t ordinal = 1; ordinal <= 5; ++ordinal) {
        table.setMesh(guidOf(ordinal), bindingOf(static_cast<std::uint32_t>(ordinal), 1));
    }

    SUBCASE("first") {
        table.removeMesh(guidOf(1));
        CHECK(table.meshCount() == 4);
        CHECK(table.findMesh(guidOf(1)) == nullptr);
        CHECK(table.findMesh(guidOf(2)) != nullptr);
        CHECK(table.findMesh(guidOf(5)) != nullptr);
    }
    SUBCASE("middle") {
        table.removeMesh(guidOf(3));
        CHECK(table.meshCount() == 4);
        CHECK(table.findMesh(guidOf(3)) == nullptr);
        CHECK(table.findMesh(guidOf(2)) != nullptr);
        CHECK(table.findMesh(guidOf(4)) != nullptr);
    }
    SUBCASE("last") {
        table.removeMesh(guidOf(5));
        CHECK(table.meshCount() == 4);
        CHECK(table.findMesh(guidOf(5)) == nullptr);
        CHECK(table.findMesh(guidOf(4)) != nullptr);
    }
}

TEST_CASE("scene_render: removing an absent or nil guid changes nothing (AB8)") {
    AssetBindingTable table;
    table.setMesh(guidOf(2), bindingOf(2, 1));
    table.setMaterial(guidOf(2), materialHandleOf(2));

    table.removeMesh(guidOf(99));
    table.removeMesh(Guid{});
    table.removeMaterial(guidOf(99));
    table.removeMaterial(Guid{});

    CHECK(table.meshCount() == 1);
    CHECK(table.materialCount() == 1);
    CHECK(table.findMesh(guidOf(2)) != nullptr);
    CHECK(table.findMaterial(guidOf(2)) == materialHandleOf(2));
}

TEST_CASE("scene_render: clear empties both vectors (AB9)") {
    AssetBindingTable table;
    table.setMesh(guidOf(1), bindingOf(1, 2));
    table.setMesh(guidOf(2), bindingOf(2, 2));
    table.setMaterial(guidOf(3), materialHandleOf(3));
    REQUIRE(table.meshCount() == 2);
    REQUIRE(table.materialCount() == 1);

    table.clear();

    CHECK(table.meshCount() == 0);
    CHECK(table.materialCount() == 0);
    CHECK(table.findMesh(guidOf(1)) == nullptr);
    CHECK_FALSE(table.findMaterial(guidOf(3)).valid());
}

TEST_CASE("scene_render: the material half repeats the mesh half's key rows (AB10)") {
    AssetBindingTable table;

    SUBCASE("set/find round trip") {
        table.setMaterial(guidOf(6), materialHandleOf(60));
        CHECK(table.findMaterial(guidOf(6)) == materialHandleOf(60));
        CHECK(table.materialCount() == 1);
    }
    SUBCASE("replace, not duplicate") {
        table.setMaterial(guidOf(6), materialHandleOf(60));
        table.setMaterial(guidOf(6), materialHandleOf(61));
        CHECK(table.materialCount() == 1);
        CHECK(table.findMaterial(guidOf(6)) == materialHandleOf(61));
    }
    SUBCASE("descending insertion stays sorted") {
        for (std::uint64_t ordinal = 6; ordinal >= 1; --ordinal) {
            table.setMaterial(guidOf(ordinal), materialHandleOf(static_cast<std::uint32_t>(ordinal)));
        }
        REQUIRE(table.materialCount() == 6);
        for (std::uint64_t ordinal = 1; ordinal <= 6; ++ordinal) {
            CHECK(table.findMaterial(guidOf(ordinal)) == materialHandleOf(static_cast<std::uint32_t>(ordinal)));
        }
    }
    SUBCASE("remove") {
        table.setMaterial(guidOf(1), materialHandleOf(1));
        table.setMaterial(guidOf(2), materialHandleOf(2));
        table.removeMaterial(guidOf(1));
        CHECK(table.materialCount() == 1);
        CHECK_FALSE(table.findMaterial(guidOf(1)).valid());
        CHECK(table.findMaterial(guidOf(2)) == materialHandleOf(2));
    }
    SUBCASE("an absent guid answers an INVALID handle, not a stale one") {
        table.setMaterial(guidOf(1), materialHandleOf(1));
        CHECK_FALSE(table.findMaterial(guidOf(2)).valid());
    }
}

TEST_CASE("scene_render: the two tables are independent (AB11)") {
    AssetBindingTable table;
    table.setMesh(guidOf(5), bindingOf(5, 1));
    table.setMaterial(guidOf(5), materialHandleOf(50));

    table.removeMesh(guidOf(5));
    CHECK(table.meshCount() == 0);
    CHECK(table.findMaterial(guidOf(5)) == materialHandleOf(50));  // the same guid, the other half

    table.setMesh(guidOf(5), bindingOf(6, 1));
    table.removeMaterial(guidOf(5));
    CHECK(table.materialCount() == 0);
    REQUIRE(table.findMesh(guidOf(5)) != nullptr);
    CHECK(table.findMesh(guidOf(5))->mesh == meshHandleOf(6));
}

TEST_CASE("scene_render: a binding's submesh payload rides through verbatim (AB12)") {
    AssetBindingTable table;
    MeshBinding binding;
    binding.mesh = meshHandleOf(3);
    binding.submeshes.push_back(MeshBindingSubmesh{0U, 0U, materialHandleOf(11)});
    binding.submeshes.push_back(MeshBindingSubmesh{1U, 2U, MaterialHandle{}});
    binding.submeshes.push_back(MeshBindingSubmesh{2U, 2U, materialHandleOf(13)});
    table.setMesh(guidOf(8), binding);

    const MeshBinding* found = table.findMesh(guidOf(8));
    REQUIRE(found != nullptr);
    REQUIRE(found->submeshes.size() == 3);
    CHECK(found->submeshes[0].sourceMeshIndex == 0U);
    CHECK(found->submeshes[0].material == materialHandleOf(11));
    CHECK(found->submeshes[1].sourceMeshIndex == 2U);
    CHECK_FALSE(found->submeshes[1].material.valid());  // an INVALID handle is legal and is preserved
    CHECK(found->submeshes[2].submesh == 2U);
    CHECK(found->submeshes[2].material == materialHandleOf(13));
}

TEST_CASE("scene_render: N present keys and N+1 absent keys interleaved between them (AB13)") {
    // The case an off-by-one binary search reddens (S17): every EVEN ordinal is present, every ODD
    // ordinal sits strictly between two present keys (or outside the range at both ends), and each is
    // probed. A lookup that answers "the neighbour" rather than "the match" fails here and only here.
    AssetBindingTable table;
    constexpr std::uint64_t COUNT = 12;
    for (std::uint64_t k = 1; k <= COUNT; ++k) {
        table.setMesh(guidOf(2 * k), bindingOf(static_cast<std::uint32_t>(2 * k), 1));
        table.setMaterial(guidOf(2 * k), materialHandleOf(static_cast<std::uint32_t>(2 * k)));
    }
    REQUIRE(table.meshCount() == COUNT);
    REQUIRE(table.materialCount() == COUNT);

    for (std::uint64_t k = 1; k <= COUNT; ++k) {
        const MeshBinding* found = table.findMesh(guidOf(2 * k));
        REQUIRE(found != nullptr);
        CHECK(found->mesh == meshHandleOf(static_cast<std::uint32_t>(2 * k)));
        CHECK(table.findMaterial(guidOf(2 * k)) == materialHandleOf(static_cast<std::uint32_t>(2 * k)));
    }
    for (std::uint64_t k = 0; k <= COUNT; ++k) {
        const std::uint64_t absent = (2 * k) + 1;  // 1, 3, ..., 25 -- never inserted
        CHECK(table.findMesh(guidOf(absent)) == nullptr);
        CHECK_FALSE(table.findMaterial(guidOf(absent)).valid());
    }
}

TEST_CASE("scene_render: the table's moves stay noexcept (AB14)") {
    // Restated here, not merely in the header, so a future storage change (a hash container, a node
    // container) reddens in the case that owns it rather than at a distant SceneRenderer terminate.
    static_assert(std::is_nothrow_move_constructible_v<AssetBindingTable>);
    static_assert(std::is_nothrow_move_assignable_v<AssetBindingTable>);
    CHECK(std::is_nothrow_move_constructible_v<AssetBindingTable>);
    CHECK(std::is_nothrow_move_assignable_v<AssetBindingTable>);
}

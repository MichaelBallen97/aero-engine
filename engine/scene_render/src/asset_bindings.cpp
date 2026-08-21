// engine/scene_render/src/asset_bindings.cpp -- task 3.1.5: the GUID -> renderer-handle table.
// Every method is one std::lower_bound over a sorted vector, comparing on `.first` with
// Guid::operator<. There is no second lookup shape anywhere in this file.
//
// Measured at task 3.1.5's sabotage pass, correcting what this comment used to claim: substituting
// std::upper_bound is NOT a one-token edit and does NOT have a single witness. ByGuid is the
// PARTITIONING form -- it takes (entry, guid) -- while upper_bound invokes comp(value, *it) with the
// arguments reversed, so the literal substitution is a hard compile error inside <bits/stl_algo.h>
// rather than a behaviour change. Forced through with a reversed lambda it reddens 18 cases across
// AB* and BR*, not one. The comparator's shape is therefore load-bearing: it makes the wrong lookup
// unspellable, which is a stronger guarantee than any test in this file provides.

#include <aero/scene_render/asset_bindings.hpp>

#include <algorithm>
#include <utility>

namespace engine::scene_render {

namespace {

// The one comparator, so the sort order of both vectors is stated exactly once.
struct ByGuid {
    template <typename Pair>
    [[nodiscard]] bool operator()(const Pair& entry, Guid guid) const noexcept {
        return entry.first < guid;
    }
};

}  // namespace

void AssetBindingTable::setMesh(Guid guid, MeshBinding binding) {
    if (!guid.valid()) {
        return;  // a nil guid is the NONE sentinel, never a key (guid.hpp)
    }
    const auto it = std::lower_bound(meshes.begin(), meshes.end(), guid, ByGuid{});
    if (it != meshes.end() && it->first == guid) {
        it->second = std::move(binding);  // REPLACES wholesale -- never a second entry for one guid
        return;
    }
    meshes.insert(it, std::pair<Guid, MeshBinding>{guid, std::move(binding)});
}

void AssetBindingTable::removeMesh(Guid guid) noexcept {
    const auto it = std::lower_bound(meshes.begin(), meshes.end(), guid, ByGuid{});
    if (it != meshes.end() && it->first == guid) {
        meshes.erase(it);
    }
    // absent (and nil, which can never be present) is a silent no-op
}

const MeshBinding* AssetBindingTable::findMesh(Guid guid) const noexcept {
    if (!guid.valid()) {
        return nullptr;  // stated, not derived from setMesh's refusal: the two must not drift
    }
    const auto it = std::lower_bound(meshes.begin(), meshes.end(), guid, ByGuid{});
    return it != meshes.end() && it->first == guid ? &it->second : nullptr;
}

void AssetBindingTable::setMaterial(Guid guid, render::MaterialHandle handle) {
    if (!guid.valid()) {
        return;
    }
    const auto it = std::lower_bound(materials.begin(), materials.end(), guid, ByGuid{});
    if (it != materials.end() && it->first == guid) {
        it->second = handle;
        return;
    }
    materials.insert(it, std::pair<Guid, render::MaterialHandle>{guid, handle});
}

void AssetBindingTable::removeMaterial(Guid guid) noexcept {
    const auto it = std::lower_bound(materials.begin(), materials.end(), guid, ByGuid{});
    if (it != materials.end() && it->first == guid) {
        materials.erase(it);
    }
}

render::MaterialHandle AssetBindingTable::findMaterial(Guid guid) const noexcept {
    if (!guid.valid()) {
        return render::MaterialHandle{};
    }
    const auto it = std::lower_bound(materials.begin(), materials.end(), guid, ByGuid{});
    return it != materials.end() && it->first == guid ? it->second : render::MaterialHandle{};
}

void AssetBindingTable::clear() noexcept {
    meshes.clear();
    materials.clear();
}

std::size_t AssetBindingTable::meshCount() const noexcept { return meshes.size(); }

std::size_t AssetBindingTable::materialCount() const noexcept { return materials.size(); }

}  // namespace engine::scene_render

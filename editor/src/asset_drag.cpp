// editor/src/asset_drag.cpp -- task 3.1.5: the asset drag payload's decode and the drop routing
// matrix. PURE: no ImGui call anywhere in this TU, no disk, no GPU, no logging. The panels do the
// BeginDragDropSource / AcceptDragDropPayload glue and hand the raw bytes here.
#include <aero/editor/asset_drag.hpp>

#include <cstring>
#include <optional>
#include <string_view>

namespace engine::editor {

std::optional<AssetDragPayload> decodeAssetDragPayload(const void* data, int sizeBytes) noexcept {
    if (data == nullptr || sizeBytes != static_cast<int>(sizeof(AssetDragPayload))) {
        return std::nullopt;
    }
    // VALUE-init: every MEMBER is deterministic before the copy. It does NOT zero the tail padding --
    // see the header's measured note -- and nothing here reads it.
    AssetDragPayload out{};
    std::memcpy(&out, data, sizeof(out));
    if (!out.guid.valid()) {
        return std::nullopt;  // a nil guid in a payload is a CORRUPT payload, never a "none" value
    }
    return out;
}

bool assetKindIsDraggable(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Texture:
        case AssetKind::Model:
        case AssetKind::Material:
            return true;
        case AssetKind::Folder:
        case AssetKind::Audio:
        case AssetKind::Text:
        case AssetKind::Unknown:
            return false;
    }
    return false;  // unreachable; enumerated so a new AssetKind is a -Wswitch warning, not silent
}

std::string_view dropSurfaceLabel(DropSurface surface) noexcept {
    switch (surface) {
        case DropSurface::HierarchyRow:
            return "hierarchy row";
        case DropSurface::HierarchyVoid:
            return "hierarchy void";
        case DropSurface::Viewport:
            return "viewport";
        case DropSurface::MaterialSlot:
            return "material slot";
    }
    return "hierarchy row";  // unreachable; enumerated so a new DropSurface is a -Wswitch warning
}

std::string_view dropActionLabel(DropAction action) noexcept {
    switch (action) {
        case DropAction::None:
            return "none";
        case DropAction::InstantiateModel:
            return "instantiate model";
        case DropAction::AssignMaterial:
            return "assign material";
        case DropAction::BindTextureSlot:
            return "bind texture slot";
    }
    return "none";  // unreachable; enumerated so a new DropAction is a -Wswitch warning
}

DropAction classifyAssetDrop(AssetKind kind, DropSurface surface, bool targetHasMeshRenderer) noexcept {
    switch (kind) {
        case AssetKind::Model:
            switch (surface) {
                case DropSurface::HierarchyRow:
                case DropSurface::HierarchyVoid:
                case DropSurface::Viewport:
                    return DropAction::InstantiateModel;
                case DropSurface::MaterialSlot:
                    return DropAction::None;
            }
            return DropAction::None;
        case AssetKind::Material:
            switch (surface) {
                // The Viewport's row means "the entity under the cursor, picked this frame, has a
                // MeshRenderer" -- one live pick per frame while hovering with a material payload.
                case DropSurface::HierarchyRow:
                case DropSurface::Viewport:
                    return targetHasMeshRenderer ? DropAction::AssignMaterial : DropAction::None;
                case DropSurface::HierarchyVoid:
                case DropSurface::MaterialSlot:
                    return DropAction::None;
            }
            return DropAction::None;
        case AssetKind::Texture:
            switch (surface) {
                case DropSurface::MaterialSlot:
                    return DropAction::BindTextureSlot;
                case DropSurface::HierarchyRow:
                case DropSurface::HierarchyVoid:
                case DropSurface::Viewport:
                    return DropAction::None;
            }
            return DropAction::None;
        case AssetKind::Folder:
        case AssetKind::Audio:
        case AssetKind::Text:
        case AssetKind::Unknown:
            return DropAction::None;
    }
    return DropAction::None;  // unreachable; both switches are total over their enums
}

}  // namespace engine::editor

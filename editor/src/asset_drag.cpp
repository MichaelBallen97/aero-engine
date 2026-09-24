// editor/src/asset_drag.cpp -- task 3.1.5: the asset drag payload's decode and the drop routing
// matrix. PURE: no ImGui call anywhere in this TU, no disk, no GPU, no logging. The panels do the
// BeginDragDropSource / AcceptDragDropPayload glue and hand the raw bytes here.
#include <aero/editor/asset_actions.hpp>  // task E.4.3 -- validateRelativeAssetPath, the decode's
#include <aero/editor/asset_drag.hpp>
// last rung. NO CYCLE: asset_actions.hpp includes
// asset_meta.hpp and project_files.hpp, and neither
// includes this file.

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
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

// task E.4.3: the second payload's decode. The order of the rungs is load-bearing: the explicit
// `payload.length >= MAX_MOVE_PAYLOAD_PATH` test is kept AS WELL AS assetMovePayloadFits, because the
// bound must be checked BEFORE payload.path[payload.length] is indexed -- an out-of-range index would
// be UB, and UBSan would catch it only on a Debug lane. The shared predicate then re-asserts the same
// bound over the view, which is the comparator both sides share. Redundant by one comparison and
// correct by construction.
std::optional<std::string> decodeAssetMoveDragPayload(const void* data, int sizeBytes) {
    if (data == nullptr || sizeBytes != static_cast<int>(sizeof(AssetMoveDragPayload))) {
        return std::nullopt;
    }
    AssetMoveDragPayload payload{};
    std::memcpy(&payload, data, sizeof payload);  // NEVER a cast: ImGui's buffer is alignas(1)
    if (payload.length == 0U || payload.length >= MAX_MOVE_PAYLOAD_PATH) {
        return std::nullopt;
    }
    if (payload.path[payload.length] != '\0') {
        return std::nullopt;  // the NUL must be INSIDE the buffer, at exactly that index
    }
    const std::string_view view(payload.path.data(), payload.length);
    if (!assetMovePayloadFits(view)) {
        return std::nullopt;  // the SAME comparator the encoder refuses with
    }
    if (validateRelativeAssetPath(view) != AssetOpRefusal::None) {
        // A payload carrying an escaping path is a CORRUPT payload, not a refusable operation.
        return std::nullopt;
    }
    return std::string(view);
}

bool assetKindIsDraggable(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Texture:
        case AssetKind::Model:
        case AssetKind::Material:
        // task E.3.3: Audio joins the set, because AudioSource::clip is a drop target now and a
        // target whose payload can never be dragged is a dead surface -- the SOURCE refuses to start
        // a drag for a non-draggable kind (asset_browser_panel.cpp). Its four pre-existing matrix
        // rows are already None, so nothing outside the new surface changes behaviour.
        case AssetKind::Audio:
            return true;
        case AssetKind::Folder:
        case AssetKind::Text:
        case AssetKind::Unknown:
            return false;
    }
    return false;  // unreachable; enumerated so a new AssetKind is a -Wswitch warning, not silent
}

namespace {

// ASCII fold by hand, never std::tolower: that one is LOCALE-dependent and takes an int whose value
// must be representable as unsigned char (3.1.3's S17 finding). Two bytes at a time, no allocation.
[[nodiscard]] bool asciiEqualsFolded(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    const auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (fold(a[i]) != fold(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<AssetKind> assetReferenceKindFromToken(std::string_view token) noexcept {
    // DERIVED from the two sentences that already exist -- the filter options array and the draggable
    // predicate -- so a kind added to one and not the other is AR5 going red rather than a silent
    // asymmetry. Never a hand-written list; that list would be the third place to keep in step.
    for (const AssetKind kind : ASSET_KIND_FILTER_OPTIONS) {
        if (assetKindIsDraggable(kind) && asciiEqualsFolded(token, assetKindLabel(kind))) {
            return kind;
        }
    }
    return std::nullopt;
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
        case DropSurface::AssetField:
            return "asset field";
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
        case DropAction::AssignAssetReference:
            return "assign asset reference";
    }
    return "none";  // unreachable; enumerated so a new DropAction is a -Wswitch warning
}

DropAction classifyAssetDrop(AssetKind kind, DropSurface surface, bool targetHasMeshRenderer,
                             std::optional<AssetKind> fieldKind) noexcept {
    // task E.3.3: the AssetField row is ONE expression, written identically inside every kind's case
    // rather than hoisted above the switch, so BOTH switches stay total and `default:`-free and
    // draggability is COMPOSED rather than restated. It is what makes Folder/Text/Unknown answer None
    // there (they are not draggable), which keeps DR8's "a refused kind is None whatever the surface"
    // true by construction -- and what makes a future Text reference ONE edit: widen
    // assetKindIsDraggable and this row, the token vocabulary and the source-side refusal widen
    // together.
    const auto assetFieldRow = [kind, fieldKind]() {
        return assetKindIsDraggable(kind) && (!fieldKind.has_value() || *fieldKind == kind)
                   ? DropAction::AssignAssetReference
                   : DropAction::None;
    };
    switch (kind) {
        case AssetKind::Model:
            switch (surface) {
                case DropSurface::HierarchyRow:
                case DropSurface::HierarchyVoid:
                case DropSurface::Viewport:
                    return DropAction::InstantiateModel;
                case DropSurface::MaterialSlot:
                    return DropAction::None;
                case DropSurface::AssetField:
                    return assetFieldRow();
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
                case DropSurface::AssetField:
                    return assetFieldRow();
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
                case DropSurface::AssetField:
                    return assetFieldRow();
            }
            return DropAction::None;
        // THE FOUR KINDS THE OLD SURFACES ALL REFUSE, in ONE arm -- and Audio (task E.3.3, draggable)
        // sits here with the three that are not, deliberately. On the four pre-E.3.3 surfaces they are
        // identical: None, every time. The ONLY thing that separates them is DRAGGABILITY, and
        // draggability is decided INSIDE assetFieldRow() -- so merging the arms is what makes that a
        // structural statement rather than a comment, and it is why they reach the composed row
        // instead of short-circuiting to None.
        //
        // That composition is the whole point: the day a script component wants a `.json`, widening
        // assetKindIsDraggable is ONE edit and this surface widens with it. A direct
        // `return DropAction::None;` here would read identically today and would silently NOT widen.
        // AR2's Text-on-a-Text-field row is what proves the term is live.
        case AssetKind::Audio:
        case AssetKind::Folder:
        case AssetKind::Text:
        case AssetKind::Unknown:
            switch (surface) {
                case DropSurface::HierarchyRow:
                case DropSurface::HierarchyVoid:
                case DropSurface::Viewport:
                case DropSurface::MaterialSlot:
                    return DropAction::None;
                case DropSurface::AssetField:
                    return assetFieldRow();
            }
            return DropAction::None;
    }
    return DropAction::None;  // unreachable; both switches are total over their enums
}

}  // namespace engine::editor

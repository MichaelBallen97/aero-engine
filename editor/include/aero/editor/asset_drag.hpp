#pragma once
// Aero Engine -- the asset drag payload and the drop routing matrix (task 3.1.5). PUBLIC and PURE:
// this header names no ImGui type, and asset_drag.cpp calls no ImGui function -- the panels do the
// Begin/Accept glue and hand the decoded bytes here. That split is what makes the whole accept/refuse
// matrix a tier-0 table test while the ImGui half stays four lines per site.
//
// This is the tree's SECOND payload type. The first is the Hierarchy's own "AERO_ENTITY" reparent
// payload (hierarchy_panel.cpp), which is untouched: the two type strings differ, so IsDataType
// refuses each other's payloads and the two features cannot cross-fire.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>          // Vec2 -- ViewportAssetDrop's NDC point
#include <aero/editor/asset_view.hpp>  // AssetKind
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace engine::editor {

inline constexpr const char* ASSET_PAYLOAD_TYPE = "AERO_ASSET";  // <= 32 chars (ImGui's own limit)

struct AssetDragPayload {
    Guid guid;              // NEVER nil in a live payload -- the source refuses to start otherwise
    std::uint8_t kind = 0;  // static_cast<std::uint8_t>(AssetKind). A TARGET-SIDE PEEK HINT ONLY:
                            // transient, never persisted, and every accepted drop re-resolves the
                            // guid against the live database before acting. A record can vanish
                            // mid-drag, so the payload is a hint and the database is the authority.
};
// 24 bytes = 16 (Guid) + 1 (kind) + 7 bytes of TAIL PADDING. Always VALUE-initialise before filling
// (`AssetDragPayload p{};` then assign) rather than aggregate-filling {guid, kind}: every MEMBER is
// then deterministic whatever a future field append does.
//
// MEASURED, and it CORRECTS the plan's own claim that value-initialisation also zeroes the padding
// ("the fix is one character"): it does not, and it CANNOT for this type. [dcl.init] value-init means
// zero-initialise-then-default-initialise only where the default constructor is non-trivial, and
// compilers elide the whole-object zeroing when the constructor writes every member -- which is
// exactly this case, because engine::Guid carries `hi = 0` / `lo = 0` NSDMIs and therefore makes
// AssetDragPayload NOT trivially default constructible. Probed directly on Apple clang 21 at -O0
// against a poisoned stack slot: a trivially-default-constructible twin comes back all-zero, this
// type comes back with seven garbage tail bytes. Removing the `kind = 0` initialiser does not help --
// the Guid member alone decides it.
//
// NOTHING DEPENDS ON THOSE BYTES and DR14 pins that: the decode memcpy's the whole object and reads
// only `guid` and `kind`, so an indeterminate-padding payload and a zero-padding one decode
// identically. Reading them back is well-defined (unsigned char access). If a future MSan lane or a
// byte-exact assertion ever needs the 24 bytes deterministic, the fix belongs at the ONE
// SetDragDropPayload call site -- copy into an explicitly zeroed byte buffer there -- never here.
static_assert(sizeof(AssetDragPayload) == 24);
static_assert(alignof(AssetDragPayload) == 8);
static_assert(std::is_trivially_copyable_v<AssetDragPayload>);

// THE ONLY READER OF ImGuiPayload::Data IN THIS TREE. Takes the raw pointer + size so this header names
// no ImGui type; the caller passes payload->Data and payload->DataSize after its own IsDataType check.
// std::memcpy into a local, NEVER a cast: ImGui's buffer is alignas(1) (and, at 24 bytes, is the HEAP
// buffer rather than the inline 16-byte one), and the Debug lanes run UBSan.
// nullopt for: null data, a size that is not EXACTLY sizeof(AssetDragPayload), or a nil guid after the
// copy -- a nil guid in a payload is a corrupt payload, not a "none" value.
[[nodiscard]] std::optional<AssetDragPayload> decodeAssetDragPayload(const void* data, int sizeBytes) noexcept;

// Model | Texture | Material. Folder, Audio, Text and Unknown are NOT draggable -- and note that a
// `.mtl` classifies Unknown (asset_view.cpp's table) even though it is importable, so it starts no drag.
[[nodiscard]] bool assetKindIsDraggable(AssetKind kind) noexcept;

// ---- the routing matrix (D12) --------------------------------------------------------------------

enum class DropSurface : std::uint8_t { HierarchyRow = 0, HierarchyVoid, Viewport, MaterialSlot };
enum class DropAction : std::uint8_t { None = 0, InstantiateModel, AssignMaterial, BindTextureSlot };

// NEVER named toString (.claude/rules/ci-portability.md): DOCTEST_STRINGIFY expands to an UNQUALIFIED
// toString(...), so a toString on a public header is found by ADL and hard-errors every lane inside
// doctest.h. The material*Label / cookedMeshStatusLabel naming, inherited.
[[nodiscard]] std::string_view dropSurfaceLabel(DropSurface surface) noexcept;
[[nodiscard]] std::string_view dropActionLabel(DropAction action) noexcept;

// THE WHOLE ACCEPT/REFUSE MATRIX, as one total pure function. Every panel's ImGui glue calls this
// BEFORE AcceptDragDropPayload, so an illegal drop never draws a highlight (the peek rule,
// .claude/rules/editor.md). `targetHasMeshRenderer` is FALSE for every surface that has no target
// entity (the void, a material slot) and is recomputed from the LIVE World at the accept site -- never
// remembered, never taken from the payload.
//
//   kind \ surface | HierarchyRow          | HierarchyVoid    | Viewport              | MaterialSlot
//   Model          | InstantiateModel      | InstantiateModel | InstantiateModel      | None
//   Material       | AssignMaterial iff MR | None             | AssignMaterial iff MR | None
//   Texture        | None                  | None             | None                  | BindTextureSlot
//   Folder/Audio/Text/Unknown              | None everywhere
//
// Implemented as a switch (kind) containing a switch (surface), BOTH without `default:` -- so a new
// AssetKind or a new DropSurface is a -Wswitch error rather than a silent None.
[[nodiscard]] DropAction classifyAssetDrop(AssetKind kind, DropSurface surface, bool targetHasMeshRenderer) noexcept;

// ---- the three drop-request structs (0.4) --------------------------------------------------------
// One struct per surface, each carrying EXACTLY what the real gesture carries and nothing more. They
// live here, on the PUBLIC pure header, so the panels' src-private headers and the tests share ONE
// definition. Each panel stores std::optional<T> and exposes a one-shot taker.

struct HierarchyAssetDrop {
    AssetDragPayload payload;
    Entity targetRow{};  // Entity{} == the void target (drop at scene root)
};

struct ViewportAssetDrop {
    AssetDragPayload payload;
    // NDC, y UP -- picking.hpp's own convention, and what viewportRay takes. NOT screen points: NDC is
    // resolution-independent and is what a test can spell without knowing the panel's pixel geometry.
    Vec2 ndc{};
};

struct MaterialSlotTextureDrop {
    std::size_t slot = 0;  // 0..MATERIAL_TEXTURE_SLOT_COUNT-1
    Guid textureGuid;
};

}  // namespace engine::editor

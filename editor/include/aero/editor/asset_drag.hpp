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

#include <array>  // task E.4.3 -- AssetMoveDragPayload's fixed-capacity path
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>  // task E.4.3 -- decodeAssetMoveDragPayload's return
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

// ---- task E.4.3: the move payload ---------------------------------------------------------------
// The tree's THIRD payload type, after "AERO_ENTITY" (hierarchy reparent) and "AERO_ASSET" (3.1.5).
// 15 chars, inside ImGui's own 32-char limit (imgui.h:2832 declares DataType as char[32+1] and
// SetDragDropPayload asserts ImStrlen(type) < 33).
//
// "AERO_ASSET" is a strict PREFIX of "AERO_ASSET_MOVE", which would cross-fire under any prefix
// test. It does not, and that is VERIFIED rather than assumed: ImGuiPayload::IsDataType is
// `strcmp(type, DataType) == 0` (imgui.h:2838) -- a FULL compare -- and AcceptDragDropPayload calls
// it before anything else (imgui.cpp:15859). RE-READ BOTH AT EVERY ImGui BUMP.
inline constexpr const char* ASSET_MOVE_PAYLOAD_TYPE = "AERO_ASSET_MOVE";

// 1024 is macOS's PATH_MAX, the tightest of the three desktop targets (Linux 4096; Windows 260
// without long-path opt-in). An assets-relative path at or beyond this length cannot be OPENED on
// macOS under any non-empty project root, so it cannot exist in a scanned tree there -- which makes
// this bound cover every path the browser can actually show, rather than every path MAX_TREE_DEPTH
// (32) x MAX_ASSET_NAME_BYTES (255) makes lexically legal (8192). The longest tracked path in this
// repository measures 71 bytes.
//
// 512 was rejected: a LEGAL deep path would be silently undraggable -- the drag would start, the
// target would peek nothing, and the user would see a drag that does nothing with no message
// anywhere.
inline constexpr std::size_t MAX_MOVE_PAYLOAD_PATH = 1024;  // bytes, NUL-terminated WITHIN

// For the two things that cannot ride AssetDragPayload: a FOLDER (no guid, no sidecar, not an asset)
// and a NON-DRAGGABLE kind (Text, Unknown -- a .txt, a .mtl). Fixed-capacity so the type stays
// trivially copyable and the decode stays a pure function.
//
// UNLIKE AssetDragPayload this struct has NO PADDING AT ALL (1024 + 2 + 1 + 1 == 1028, already
// 2-aligned), so a value-initialised instance is all-zero bytes with nothing indeterminate and the
// call site needs no memset. The static_asserts below PIN that, because a future field would
// silently reintroduce padding and put indeterminate bytes on the wire.
struct AssetMoveDragPayload {
    std::array<char, MAX_MOVE_PAYLOAD_PATH> path{};  // UTF-8, '/'-separated, NUL-terminated
    std::uint16_t length = 0;                        // bytes before the NUL; 0 is never valid
    std::uint8_t kind = 0;                           // static_cast<uint8_t>(AssetKind) -- a PEEK HINT
    std::uint8_t isDirectory = 0;                    // 0/1; a bool would make the padding a question
};
static_assert(sizeof(AssetMoveDragPayload) == MAX_MOVE_PAYLOAD_PATH + 4);
static_assert(alignof(AssetMoveDragPayload) == 2);
static_assert(std::is_trivially_copyable_v<AssetMoveDragPayload>);

// TRUE iff `relativePath` fits in AssetMoveDragPayload with its NUL inside. The ENCODER-side twin of
// decodeAssetMoveDragPayload's length rung: 3.7.2's rule -- two places must never compare the same
// key by different rules -- so the source's refusal and the decoder's refusal are ONE comparator, in
// ONE place, and cannot drift apart at the boundary.
//
// Refusing at source alone would leave a legal-but-deep path undraggable, which is the failure being
// removed; raising the bound alone would leave the encoder free to write a TRUNCATED path the decoder
// then accepts as a different, valid path -- silently moving the wrong file. The pair makes over-long
// unreachable AND un-truncatable.
[[nodiscard]] constexpr bool assetMovePayloadFits(std::string_view relativePath) noexcept {
    return !relativePath.empty() && relativePath.size() < MAX_MOVE_PAYLOAD_PATH;
}

// The decodeAssetDragPayload shape VERBATIM: raw pointer + size, so this header names no ImGui type,
// and std::memcpy into a local NEVER a cast -- ImGui's buffer is alignas(1) and the three Debug
// lanes run UBSan. NOT noexcept, unlike its sibling: it returns std::optional<std::string> and the
// string allocates.
//
// nullopt for: null data; a size that is not EXACTLY sizeof(AssetMoveDragPayload); a length
// assetMovePayloadFits refuses (0, or >= MAX_MOVE_PAYLOAD_PATH); a missing NUL at path[length]; or a
// path validateRelativeAssetPath rejects. That LAST rung is the important one and it is deliberate:
// a payload carrying an escaping path is a CORRUPT payload, not a refusable operation -- the same
// nil-guid-is-corrupt posture decodeAssetDragPayload already takes, applied to the other payload.
[[nodiscard]] std::optional<std::string> decodeAssetMoveDragPayload(const void* data, int sizeBytes);

// Model | Texture | Material | AUDIO (task E.3.3 -- AudioSource::clip is a drop target now, so an
// audio file must be able to START a drag; the source refuses a non-draggable kind). Folder, Text and
// Unknown are NOT draggable -- and note that a `.mtl` classifies Unknown (asset_view.cpp's table) even
// though it is importable, so it starts no drag.
[[nodiscard]] bool assetKindIsDraggable(AssetKind kind) noexcept;

// task E.3.3: the token an AERO_ASSET(...) may carry -> the kind, DERIVED and never listed: the
// DRAGGABLE kind whose ASCII-folded assetKindLabel equals the ASCII-folded token. "texture" "model"
// "material" "audio" resolve; "folder" "text" "unknown" and anything else are nullopt, which the
// widget treats as UNCONSTRAINED plus one WARN. The kinds a field may NAME are exactly the kinds a
// drag may CARRY -- one sentence, one home, and AR5 pins the derivation in both directions.
[[nodiscard]] std::optional<AssetKind> assetReferenceKindFromToken(std::string_view token) noexcept;

// ---- the routing matrix (D12) --------------------------------------------------------------------
//
// AssetField (task E.3.3) is APPENDED, so MaterialSlot's value is unmoved. Neither enum is persisted
// anywhere, but asset_drag_test.cpp's ALL_SURFACES is POSITIONAL, and this sentence is what says so.

enum class DropSurface : std::uint8_t { HierarchyRow = 0, HierarchyVoid, Viewport, MaterialSlot, AssetField };
enum class DropAction : std::uint8_t {
    None = 0,
    InstantiateModel,
    AssignMaterial,
    BindTextureSlot,
    AssignAssetReference
};

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
//   kind \ surface | HierarchyRow  | HierarchyVoid | Viewport      | MaterialSlot | AssetField
//   Model          | Instantiate   | Instantiate   | Instantiate   | None         | AssignRef*
//   Material       | Assign iff MR | None          | Assign iff MR | None         | AssignRef*
//   Texture        | None          | None          | None          | BindSlot     | AssignRef*
//   Audio          | None          | None          | None          | None         | AssignRef*
//   Folder/Text/Unknown            | None everywhere, AssetField INCLUDED (not draggable)
//
//   (*) iff !fieldKind || *fieldKind == kind.
//
// `fieldKind` is the kind the field's AERO_ASSET names -- nullopt for an unannotated field AND for
// every surface that is not a field. NON-DEFAULTED (E.1.3's rule): a default would let a future
// AssetField site forget it and silently become UNCONSTRAINED, a wrong picture with no error and no
// failing test, while non-defaulted makes every unconverted site a compile error.
//
// Implemented as a switch (kind) containing a switch (surface), BOTH without `default:` -- so a new
// AssetKind or a new DropSurface is a -Wswitch error rather than a silent None.
[[nodiscard]] DropAction classifyAssetDrop(AssetKind kind, DropSurface surface, bool targetHasMeshRenderer,
                                           std::optional<AssetKind> fieldKind) noexcept;

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

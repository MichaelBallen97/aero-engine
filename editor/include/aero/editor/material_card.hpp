#pragma once
// Aero Engine -- everything a material's BROWSER CARD decides (task E.4.5). PUBLIC and PURE: no ImGui, no
// GPU, no <filesystem>, no logging, no EnTT. Every function below is provable from a MaterialDocument
// literal and string literals with no context of any kind -- which is what lets tier 0 walk the whole
// presentation rule while the four hosts (the browser's grid and list, the picker's popup grid and the
// Inspector's Guid row) hold only the glue.
//
// THE RIG BELOW IS WHY THE THUMBNAIL'S CACHE KEY IS COMPLETE. ThumbnailKey is {Guid, ContentHash} and
// cannot express lighting or exposure, so a thumbnail lit by the OPEN SCENE would be a picture whose inputs
// the key does not cover -- stale for ever, or re-rendered on every drag of the sun. The lighting, the
// tonemap and the orbit angle are therefore FIXED, and each is spelled exactly once, here.
#include <aero/core/math.hpp>
#include <aero/editor/asset_view.hpp>            // IconColor
#include <aero/editor/material_preview_rig.hpp>  // MaterialPreviewLighting (task E.2.4, by name)
#include <aero/reflect/material_format.hpp>      // MaterialDocument
#include <aero/render/tonemap.hpp>               // TonemapParams

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

// ---- the fixed studio rig ---------------------------------------------------------------------------
// A neutral studio: a cool sky over a warm dark ground, and a key light over the viewer's LEFT shoulder
// for MATERIAL_THUMBNAIL_ORBIT_ANGLE. EVERY NUMBER IS A TUNING CONSTANT judged on the validation page;
// tier 0 asserts RELATIONSHIPS (a sun is present, its intensity is positive and finite, its direction is
// unit length and points down, the gradient colours are finite and the sky is not the ground), never a
// magnitude, so a retune reddens nothing.
inline constexpr float MATERIAL_THUMBNAIL_ORBIT_ANGLE = 0.6F;  // radians -- a three-quarter view
[[nodiscard]] MaterialPreviewLighting materialThumbnailLighting() noexcept;
// ALREADY SANITIZED -- it is sanitizeTonemapParams(TonemapParams{}) -- because tonemapAndEncode puts that
// duty on its caller (tonemap.hpp) and materialSwatchColor below calls it directly.
[[nodiscard]] render::TonemapParams materialThumbnailTonemap() noexcept;

// ---- the display name ---------------------------------------------------------------------------------
// A HARD BYTE CAP, because MaterialDocument::name is arbitrary user-authored JSON text and the LIST ROW does
// not elide. The grid tile's own width-driven elision (elideForCaption) happens on top of this.
inline constexpr std::size_t MAX_MATERIAL_DISPLAY_NAME_BYTES = 96;
// U+2026 HORIZONTAL ELLIPSIS as its UTF-8 bytes -- the SAME character asset_tile.cpp's elideForCaption
// appends, which the UI font draws through its CP1252 remap (.claude/rules/editor.md, "The UI font").
// Spelled as bytes so no compiler's guess at the source character set can change it. One name, one
// ellipsis, on every surface.
inline constexpr std::string_view MATERIAL_DISPLAY_NAME_ELLIPSIS = "\xE2\x80\xA6";
// The list row's and the Inspector sentence's separator -- ONE spelling, so no host can drift.
inline constexpr std::string_view MATERIAL_CARD_SEPARATOR = "  -  ";

struct MaterialDisplayName {
    std::string text;        // "" == the document carried no usable name
    bool truncated = false;  // the cap cut it, so `text` ends with MATERIAL_DISPLAY_NAME_ELLIPSIS
};

// Rules 1-3 and 5: everything about a name that does NOT depend on the file it lives in. TOTAL.
//   1. every ASCII control byte (< 0x20, and 0x7F) becomes a space -- a caption is ONE line, and a newline
//      reaching ImDrawList::AddText is a real line break that paints over the next row of tiles;
//   2. runs of spaces collapse to one, and both ends are trimmed;
//   3. nothing left -> "";
//   5. longer than MAX_MATERIAL_DISPLAY_NAME_BYTES -> the longest prefix of at most that many bytes that
//      ends on a UTF-8 boundary (elideForCaption's own rule, on bytes rather than on measured width), its
//      trailing space dropped, then MATERIAL_DISPLAY_NAME_ELLIPSIS.
// Rule 4 -- "a name equal to the file's stem is noise" -- is NOT here: see materialCardSubtitle. Bytes at or
// above 0x80 pass through untouched; this never validates or re-encodes UTF-8.
[[nodiscard]] MaterialDisplayName sanitizeMaterialDisplayName(std::string_view documentName);

// Rule 4's predicate: TRUE when `name` equals `fileName`'s STEM, ASCII-case-insensitively. The stem is the
// name up to its LAST '.', unless there is none or it is the final byte ("x." has no extension, so it is
// its own stem) -- asset_view.cpp's rawExtensionOf rule. Allocation-free. THIS IS DE-DUPLICATION, NOT
// DERIVATION: nothing in this header ever fills a missing name FROM a file name, and nothing anywhere
// renames a file when its name changes (the roadmap's "without deriving either name from the other").
[[nodiscard]] bool materialNameMatchesStem(std::string_view name, std::string_view fileName) noexcept;

// ---- the swatch -------------------------------------------------------------------------------------
// The tile fill for a material whose thumbnail has not rendered, or never will. nullopt -> the caller keeps
// iconColorFor(AssetKind::Material), today's behaviour exactly.
// linear baseColorFactor.rgb -> tonemapAndEncode(..., materialThumbnailTonemap()) -> round(v * 255), so the
// swatch and the rendered sphere go through ONE tone curve and the swatch -> sphere change is not a jump.
// NON-FINITE IS A REFUSAL, NOT A CLAMP: tonemapAndEncode PROPAGATES a NaN channel (tonemap.hpp), and a NaN
// reaching IM_COL32 is a garbage byte -- so std::isfinite on x, y and z comes FIRST.
// ALPHA IS IGNORED AND THE RESULT IS ALWAYS a = 255: baseColorFactor.w is the material's opacity, and a tile
// that fades out is an identity cue nobody can see.
[[nodiscard]] std::optional<IconColor> materialSwatchColor(const MaterialDocument& document) noexcept;

// TRUE when the kind label drawn ON a swatch should be dark rather than white: relative luminance
// 0.2126 R + 0.7152 G + 0.0722 B of the DISPLAY-SPACE bytes, at or above 128. INTEGER ARITHMETIC in
// ten-thousandths, so the boundary between grey 127 and grey 128 is EXACT rather than a float's rounding.
[[nodiscard]] bool materialSwatchWantsDarkLabel(IconColor swatch) noexcept;

// ---- the card: what the cache stores, and what a host reads ---------------------------------------------
struct MaterialCard {
    std::string displayName;            // sanitised (rules 1-3 and 5); "" == no usable name
    bool displayNameTruncated = false;  // see materialCardSubtitle
    IconColor swatch{};
    bool hasSwatch = false;  // false == baseColorFactor was non-finite; the host keeps the kind colour
};
[[nodiscard]] MaterialCard materialCardFor(const MaterialDocument& document);

// Rule 4, AT DRAW TIME, against the file's CURRENT leaf -- never cached, because a rename keeps the
// ThumbnailKey (the import cache is keyed by GUID and a move carries its content hash), so an answer cached
// at read time would be about a file name that no longer exists. "" for a null card, for an empty name, and
// for an UNtruncated name equal to the leaf's stem. A TRUNCATED name is never suppressed: capped, it can no
// longer be proven equal to anything, and showing it is the harmless one of the two wrong answers.
// THE RESULT VIEWS card->displayName -- valid while the card is, i.e. until the owning cache's next
// service() or clear(), neither of which ever runs inside a draw walk.
[[nodiscard]] std::string_view materialCardSubtitle(const MaterialCard* card, std::string_view fileName) noexcept;
// The tile fill: the card's swatch, or nullopt (a null card, or no finite base colour).
[[nodiscard]] std::optional<IconColor> materialCardTint(const MaterialCard* card) noexcept;

// The list row's and the Inspector's composition. An empty subtitle -> fileName VERBATIM, which is what
// keeps every existing row byte-identical; otherwise fileName + MATERIAL_CARD_SEPARATOR + subtitle. Callers
// pass materialCardSubtitle's answer, so rule 4 has already run.
[[nodiscard]] std::string materialCardRowText(std::string_view fileName, std::string_view subtitle);

}  // namespace engine::editor

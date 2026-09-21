#pragma once
// Aero Engine — everything the Material panel's LAYOUT decides (task E.3.4). PUBLIC, PURE,
// ImGui-free, EnTT-free, GPU-free, <filesystem>-free, logging-free: every function below is provable
// from a MaterialDocument literal and six floats, with no window and no context of any kind. That is
// what lets a tier-0 case assert the panel's whole shape while the src-private panel holds only glue.
//
// THE PANEL STATES NO SECTION TITLE, NO FIELD LABEL, NO SENTENCE, NO FRACTION, NO MINIMUM AND NO
// PRIORITY ORDER. I172(a) pins that as source text -- the mechanical form asset_picker.cpp:1-11
// already uses for "the widget states no AssetKind:: literal at all".
//
// A NAME COLLISION THIS FILE INHERITS, stated because both spellings compile and one of them is
// wrong: engine::MaterialTextureSlot is the FILE FORMAT's (material_format.hpp) and
// engine::render::MaterialTextureSlot is the GPU's (render/material.hpp). Inside engine::editor an
// UNQUALIFIED MaterialTextureSlot is the FORMAT's -- material_edit.hpp's own convention, followed
// here. Every render type below is written render::-qualified.
#include <aero/core/guid.hpp>
#include <aero/reflect/material_format.hpp>  // MaterialDocument, MaterialTextureSlot
#include <aero/render/material.hpp>          // render::MATERIAL_TEXTURE_SLOT_COUNT -- aero::render is
                                             // PUBLIC on aero_editor_core (3.4.2's deviation 1)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

// ---- the fields, as ids --------------------------------------------------------------------------
// docs/09 section 11.1's TEN scalar keys, in the order that section's envelope table lists them --
// which is ALSO MaterialDocument's declaration order AND section 11.3's canonical WRITE order. The
// three agree today and MR2/MR4 are what keep them agreeing. `version` is the envelope and `textures`
// is the five slots, so these ten plus MATERIAL_TEXTURE_SLOT_COUNT slots are the WHOLE of the
// document's surface -- the property MR2 asserts, and the reason this is an enum rather than a list
// of strings.
enum class MaterialFieldId : std::uint8_t {
    Name = 0,
    BaseColorFactor,
    MetallicFactor,
    RoughnessFactor,
    EmissiveFactor,
    NormalScale,
    OcclusionStrength,
    AlphaMode,
    AlphaCutoff,
    DoubleSided,
};
inline constexpr std::size_t MATERIAL_FIELD_COUNT = 10;

// The row's own label, ASCII, and DELIBERATELY not the document key: a row inside a section titled
// "Base Color" reads "Tint", not "Base color". Globally distinct, so a PushID on the label would also
// work -- the panel keys on the id anyway. NO `default:` in the switch: an eleventh field is a
// -Wswitch error here rather than a row that silently renders as "".
[[nodiscard]] std::string_view materialFieldLabel(MaterialFieldId field) noexcept;

// ---- the sections --------------------------------------------------------------------------------
enum class MaterialSectionId : std::uint8_t {
    Material = 0,       // Name
    BaseColor,          // slot 0 + Tint
    MetallicRoughness,  // slot 1 + Metallic + Roughness
    Normal,             // slot 2 + Scale
    Occlusion,          // slot 3 + Strength
    Emission,           // slot 4 + Color
    Rendering,          // Alpha mode + Alpha cutoff + Double sided
    File,               // read-only: path, GUID, the unknown-key list
};
inline constexpr std::size_t MATERIAL_SECTION_COUNT = 8;

struct MaterialSection {
    MaterialSectionId id = MaterialSectionId::Material;
    std::string_view title;                   // the CollapsingHeader's label, ASCII
    std::optional<std::size_t> slot;          // the paired texture slot, in materialSlotAt's order
    std::span<const MaterialFieldId> fields;  // the scalar rows, in draw order
};

// THE TABLE, in draw order, as a span over file-static storage. The five sections carrying a slot do
// so in render::materialSlotAt's BINDING order, which is also docs/09 section 11.1's canonical order
// for `textures` -- so the panel's section order, the format's slot order and the renderer's binding
// order are ONE fact, asserted (MR3) rather than restated three times.
[[nodiscard]] std::span<const MaterialSection> materialSections() noexcept;

// ---- the slot row --------------------------------------------------------------------------------
enum class MaterialSlotNotice : std::uint8_t {
    None = 0,
    NotATexture,   // bound to a resolvable record whose kind is not Texture (3.4.2's AC-21)
    Unresolvable,  // bound to a non-nil guid THIS PROJECT does not know -- never "I have no project"
};
struct MaterialSlotRow {
    std::string valueText;      // the picker button's sentence. A std::string, so a caller that hands
                                // AssetFieldInputs::valueText (a string_view) a temporary's member
                                // DANGLES -- keep the row in a named local (C12).
    bool clearEnabled = false;  // the panel spells this BeginDisabled(!clearEnabled)
    MaterialSlotNotice notice = MaterialSlotNotice::None;
    bool samplerIsDefault = true;  // decides the disclosure's "(custom)" suffix
};

// The database's answer arrives as FOUR PLAIN VALUES rather than an AssetRecord pointer, which is what
// keeps this function free of AssetDatabase, of AssetKind and of asset_view entirely. The caller owns
// one three-line resolution and this function owns the decision.
//
//   databaseAvailable : the panel HAS a database at all (databasePtr != nullptr)
//   recordFound       : that database resolved this guid
//   recordRelativePath: the resolved record's path ("" when it did not)
//   recordIsTexture   : classifyAssetKind's answer, asked by the CALLER
//
// THE TWO BOOLEANS ARE NOT INTERCHANGEABLE AND THE DIFFERENCE IS VISIBLE TO THE USER. Without a
// database the panel is not entitled to say "this GUID is not in this project" -- it has not read one.
// That is today's behaviour (material_panel.cpp's slot notice is gated on `database != nullptr`) and
// collapsing the two states into `!recordFound` would start asserting a project fact the editor does
// not have. MR10(c) is its only witness; no case that existed before E.3.4 could see it.
[[nodiscard]] MaterialSlotRow materialSlotRow(const std::optional<MaterialTextureSlot>& slot,
                                              std::string_view recordRelativePath, bool recordIsTexture,
                                              bool databaseAvailable, bool recordFound);

// The notice's sentence, ASCII, "" for None. VERBATIM the two strings material_panel.cpp ships today,
// so a reader of the diff sees a MOVE rather than a re-wording.
[[nodiscard]] std::string_view materialSlotNoticeText(MaterialSlotNotice notice) noexcept;

// TRUE iff every sampler token equals the FORMAT's own default (docs/09 section 11.1's slot table):
// uvSet 0, wrapU/wrapV repeat, minFilter/magFilter linear, mipFilter linear. Spelled as a comparison
// against a DEFAULT-CONSTRUCTED MaterialTextureSlot's tokens -- never a hand-written list -- so a
// seventh token follows the struct automatically. The GUID is deliberately NOT compared: a slot is
// "custom" because of how it is SAMPLED, not because of what it points at.
[[nodiscard]] bool materialSlotSamplerIsDefault(const MaterialTextureSlot& slot) noexcept;

// The disclosure's label: "Sampler" or "Sampler (custom)". One place, so the two cannot drift.
//
// EVERY string_view THIS HEADER RETURNS IS A VIEW OVER A STRING LITERAL AND IS THEREFORE
// NUL-TERMINATED -- materialFieldLabel, materialSlotNoticeText, materialColorSpaceNote,
// materialDirtyWord, this, and MaterialSection::title. THAT IS A CONTRACT, NOT AN ACCIDENT: the panel
// passes `.data()` straight to ImGui, which takes a const char*. A future arm returning a view into a
// std::string breaks every call site SILENTLY, which is why the contract is stated here rather than
// left to be inferred from the implementation.
[[nodiscard]] std::string_view materialSamplerNodeLabel(bool samplerIsDefault) noexcept;

// ---- the colour-space note (D4) -------------------------------------------------------------------
// COMPOSED from materialSlotIsSrgb, never a third table: two sentences chosen by one existing
// predicate. MR6 asserts the agreement over all five indices, so the day a sixth slot exists the note
// follows the predicate rather than a list. The American spelling matches the panel's own shipping
// label and this task does not sweep the tree's inconsistency (import_details_panel.cpp says
// "Base colour") -- re-spelling a label in a panel this task is not redesigning is a gratuitous diff.
[[nodiscard]] std::string_view materialColorSpaceNote(std::size_t slotIndex) noexcept;

// ---- the status line (D7) -------------------------------------------------------------------------
enum class MaterialStatusSeverity : std::uint8_t { Disabled = 0, Notice, Warning };
struct MaterialStatusLine {
    std::string text;  // "" when there is nothing to say -- the line is drawn ANYWAY (the always-
                       // reserved idiom, asset_picker.cpp's own), so the header's height cannot change
                       // because a notice appeared, which would resize the preview.
    MaterialStatusSeverity severity = MaterialStatusSeverity::Disabled;
};
// THE PRIORITY ORDER, in one place: invalid > external change > unknown keys > last message > empty.
// `invalidMessage` is whatever the caller already computed ("" when the document validates) -- NOT a
// predicate over a session, because a session is I/O and this file is pure. `warningCount` is
// session.warnings().size(); `lastMessage` is session.lastMessage().
[[nodiscard]] MaterialStatusLine materialStatusLine(std::string_view invalidMessage, bool externalChanged,
                                                    std::size_t warningCount, std::string_view lastMessage);

// "1 unknown key will be removed on Apply." / "N unknown keys will be removed on Apply." / "" for 0.
// Its own function because the pluralisation is the kind of thing that ships wrong and never reddens.
[[nodiscard]] std::string materialUnknownKeySummary(std::size_t warningCount);

// The footer's dirty word: "Unsaved changes" or "". It lives here rather than in the panel for the
// same reason everything else does -- D6 made it a DESIGN decision ("dirty-state legibility"), and a
// design decision that exists only inside a draw walk cannot be asserted anywhere.
[[nodiscard]] std::string_view materialDirtyWord(bool dirty) noexcept;

// ---- the geometry (D1/D8) -------------------------------------------------------------------------
// STARTING VALUES, in FONT UNITS so a DPI change moves them with the font, exactly as
// asset_picker_model.hpp's ASSET_PICKER_MIN_WIDTH_FONT does. A retune is one constant and one
// re-measure; the manual validation page is the only instrument that can judge "is the preview big
// enough to read a highlight".
inline constexpr float MATERIAL_PREVIEW_FRACTION = 0.38F;  // of the panel's whole content height
inline constexpr float MATERIAL_PREVIEW_MIN_FONT = 10.0F;  // ~130 pt at a 13 px font
inline constexpr float MATERIAL_PREVIEW_MAX_FONT = 24.0F;  // ~312 pt; see the 512 px cap note below
inline constexpr float MATERIAL_BODY_MIN_FONT = 9.0F;      // the form's minimum before the preview yields
// POINTS, not font units, DELIBERATELY: it mirrors CalcItemSize's own literal
// ImMax(4.0f, avail.y + size.y) (imgui.cpp:12344-12345), which is a raw 4 and does not scale with the
// font. A font-unit floor would drift away from the number ImGui will actually apply.
inline constexpr float MATERIAL_BODY_FLOOR_POINTS = 4.0F;
// == tileEdgeFontMultiple(TileSize::Small) TODAY (asset_view.cpp:192-196), and deliberately its OWN
// constant: a slot thumbnail is not a browser tile and must be retunable alone.
inline constexpr float MATERIAL_SLOT_THUMB_FONT_MULTIPLE = 4.0F;

struct MaterialPanelMetrics {
    float availHeight = 0.0F;      // GetContentRegionAvail().y at the TOP of the Ready arm
    float fontSize = 0.0F;         // GetFontSize()
    float frameHeight = 0.0F;      // GetFrameHeight()
    float textLineHeight = 0.0F;   // GetTextLineHeight()
    float itemSpacingY = 0.0F;     // style.ItemSpacing.y
    float separatorHeight = 1.0F;  // max(style.SeparatorSize, 1) -- READ, never assumed. See the plan's
                                   // section 4.4: 1.92.8 removed the "a 1 px Separator does not move
                                   // the cursor" hack and its own header comment still describes it.
};

// WHICH WALK THE PANEL DRAWS. `previewShown` cannot carry this: it is TRUE in both modes, which is the
// whole point of the fallback. Scrolling is the ZERO value deliberately -- a default-constructed or
// partially-written layout then means "the window scrolls", which is today's behaviour and is the one
// that cannot lose a footer.
//
// The fallback exists because the step-0 measurement found the fixed chrome at 103 points against a
// 98-point content region on a Retina dock node. A layout that answers that with a blank region breaks
// I99, I135, I171 and AC-3 on every Retina Mac while the 1x CI lanes stay green.
enum class MaterialPanelMode : std::uint8_t {
    Scrolling = 0,  // the window scrolls; NO child, nothing is pinned, footerHeight is not passed on
    FixedRegions,   // header + BeginChild(0, -footerHeight) + footer, none of which can be scrolled off
};

struct MaterialPanelLayout {
    MaterialPanelMode mode = MaterialPanelMode::Scrolling;  // the panel switches on THIS, never on a
                                                            // height comparison of its own
    float headerHeight = 0.0F;                              // identity line + spacing + preview + spacing + separator
    float previewHeight = 0.0F;  // the ImGui::Image's height in POINTS. >= MATERIAL_PREVIEW_MIN_FONT *
                                 // fontSize in BOTH modes, which is what makes AC-3 unconditional.
    bool previewShown = false;   // == (previewHeight >= 1.0F), and therefore TRUE in both modes
    float footerHeight = 0.0F;   // spacing + separator + spacing + frame + spacing + textLine -- THREE
                                 // spacings (imgui.cpp:437, :6860, :12129). Computed in both modes
                                 // because the footer always DRAWS; passed to BeginChild only in
                                 // FixedRegions.
    float bodyHeight = 0.0F;     // the PREDICTION the preview clamp is computed against, and what the
                                 // MR battery asserts. NEVER passed to BeginChild -- see below.
                                 // ZERO in Scrolling mode, where the body's height is NATURAL and
                                 // nothing reserves it: a reader that treats 0 as "no body" is wrong,
                                 // and the mode is what tells the two apart.
    float thumbEdge = 0.0F;      // the slot row's thumbnail edge, in points
};
// TOTAL. A non-finite or non-positive metric yields a FINITE, POSITIVE layout with every field >= 0,
// because a NaN reaching SetNextWindowSize or ImGui::Image is an assertion -- and std::clamp(NaN, lo,
// hi) returns NaN on libc++ (3.7.2's standing rule), so the clamps here are ordered std::min/std::max
// pairs with NEGATED comparisons, never std::clamp.
//
// THE BODY IS PASSED TO ImGui AS `-footerHeight`, NEVER AS `bodyHeight`. CalcItemSize resolves a
// negative child height as ImMax(4.0f, avail.y + size.y) (imgui.cpp:12344-12345), so ImGui's own
// remainder is authoritative and a one-pixel error here costs the child a pixel instead of clipping
// the Apply button off the bottom of the panel. `bodyHeight` exists so the preview clamp has something
// to clamp against and so a tier-0 case can assert the contract.
[[nodiscard]] MaterialPanelLayout materialPanelLayout(const MaterialPanelMetrics& metrics) noexcept;

}  // namespace engine::editor

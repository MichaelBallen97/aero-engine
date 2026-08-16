#pragma once
// engine/reflect/include/aero/reflect/material_format.hpp — task 3.4.1: material asset v1
// (`.aeromat`). The DOCUMENT layer only: the glTF 2.0 metallic-roughness parameter set plus per-slot
// sampler state, with texture references held as engine::Guid. Resolving a Guid to a loaded texture,
// and mapping the sampler tokens to an rhi::SamplerDesc, are the CONSUMER's jobs — this layer stays
// rhi-free so the JSON layer never couples to the GPU layer (docs/09 §11.4 is the normative mapping).
//
// Shaped after scene_format.{hpp,cpp} (task 1.2.3) member for member: the same five functions (parse
// from JsonValue, parse from text, validate, write, canonical writeText), the same error surface, the
// same strict-envelope / WARN-unknown-keys split, and the same one-trailing-newline canonical form
// satisfying both docs/09 §1 round-trip guarantees. docs/09-file-formats.md §11 is the normative
// schema; this header enforces it.
//
// Nothing here throws, touches disk, or logs an ERROR for file content: a rejected document returns
// exactly ONE MaterialError and ZERO warnings, and warnings exist only for unknown keys on a document
// that fully validated. Since task 3.4.2 that sentence is ASSERTABLE rather than merely documented:
// MaterialParseResult carries the warnings it also logs (see the struct below).
//
// NAMING NOTE — the ADL trap (.claude/rules/ci-portability.md). The four token-label functions below
// are named material*Label and NEVER toString. doctest's DOCTEST_STRINGIFY expands to an UNQUALIFIED
// toString(...), so an engine toString(SomeEnum) declared on a public header is found by ADL, beats
// doctest's own template, and the decomposer then feeds the result into std::string_view + const
// char* — a hard compile error on every lane, reported inside doctest.h rather than at the CHECK that
// caused it. It has already cost this repo twice (rhi::TextureFormat, assets::CookedTextureFormat).
// A label function named anything else is immune by construction; the next enum that reaches a public
// engine header should inherit this posture rather than re-discover the trap.
//
// The four enums mirror the editor's ImportedMaterial / ImportedTextureRef value sets 1:1
// (editor/include/aero/editor/model_import.hpp), so a future import-materializer is lossless. That
// correspondence is asserted in the EDITOR tier when the editor first maps them (task 3.4.2), never
// here: the golden rule keeps /editor's types out of /engine, and this task has no editor diff.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/reflect/json_reader.hpp>  // JsonParseConfig (the text overload's passthrough)
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

inline constexpr std::uint32_t MATERIAL_FORMAT_VERSION = 1;

// uvSet is stored for fidelity and capped small (docs/09 §11): a v1 consumer honours set 0 and WARNs
// otherwise (MeshVertex carries one UV set), and a value >= this cap REJECTs at parse time.
inline constexpr std::uint32_t MATERIAL_MAX_UV_SETS = 4;

enum class MaterialAlphaMode : std::uint8_t { Opaque, Mask, Blend };
enum class MaterialWrap : std::uint8_t { Repeat, Clamp, Mirror };
enum class MaterialFilter : std::uint8_t { Nearest, Linear };
enum class MaterialMipFilter : std::uint8_t { None, Nearest, Linear };

// Stable token names — the FILE's own vocabulary ("opaque", "repeat", ...) and the one used in logs
// and tests. The parser matches against these very functions, so a label edit is a FORMAT change by
// construction, never a log cosmetic. Total over every enumerator; never named toString (see above).
[[nodiscard]] std::string_view materialAlphaModeLabel(MaterialAlphaMode mode) noexcept;
[[nodiscard]] std::string_view materialWrapLabel(MaterialWrap wrap) noexcept;
[[nodiscard]] std::string_view materialFilterLabel(MaterialFilter filter) noexcept;
[[nodiscard]] std::string_view materialMipFilterLabel(MaterialMipFilter filter) noexcept;

// One bound texture slot. `guid` is REQUIRED and non-nil in a PRESENT slot: absence is spelled by
// omitting the slot entirely, never by a nil guid (the Guid none-sentinel rule, guid.hpp).
struct MaterialTextureSlot {
    Guid guid;
    std::uint32_t uvSet = 0;
    MaterialWrap wrapU = MaterialWrap::Repeat;
    MaterialWrap wrapV = MaterialWrap::Repeat;
    MaterialFilter minFilter = MaterialFilter::Linear;
    MaterialFilter magFilter = MaterialFilter::Linear;
    MaterialMipFilter mipFilter = MaterialMipFilter::Linear;

    bool operator==(const MaterialTextureSlot&) const = default;
};

// A parsed material, always at MATERIAL_FORMAT_VERSION semantics (the version lives in the FILE; parse
// rejects any other value, so an in-memory document needs no version member). Every default below is
// glTF 2.0's own, verbatim — so {"version": 1} parses to exactly MaterialDocument{}.
//
// There is deliberately NO colour-space field anywhere: sRGB-ness is carried by the referenced
// artifact's own format enumerator (docs/09 §10), and a material cannot re-declare it.
struct MaterialDocument {
    std::string name;  // informational; "" == absent
    Vec4 baseColorFactor = Vec4::one();
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    Vec3 emissiveFactor{};  // components >= 0, unbounded (HDR-legal, matching the lights' precedent)
    float normalScale = 1.0F;
    float occlusionStrength = 1.0F;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5F;
    bool doubleSided = false;
    std::optional<MaterialTextureSlot> baseColor;
    std::optional<MaterialTextureSlot> metallicRoughness;
    std::optional<MaterialTextureSlot> normal;
    std::optional<MaterialTextureSlot> occlusion;
    std::optional<MaterialTextureSlot> emissive;

    bool operator==(const MaterialDocument&) const = default;
};

// line > 0  <=>  the failure happened at the JSON stage (fields copied verbatim from JsonParseError);
// material-stage failures carry zeros and put their context (the key path) in the message.
struct MaterialError {
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::size_t offset = 0;
};

struct MaterialParseResult {
    std::optional<MaterialDocument> document;
    MaterialError error;
    // Task 3.4.2, APPENDED (never inserted): the unknown keys this document carried, one entry per key,
    // at all three levels, in the SOURCE's own member order. Each entry is exactly the sentence the
    // matching AERO_LOG_WARN carries, minus that log's own "material: " channel prefix -- ONE phrasing,
    // formatted once, so a consumer can render an entry verbatim and it can never drift from the log.
    //
    // ALWAYS EMPTY for a rejected document: the sweep is success-only, so "exactly one error and zero
    // warnings" stops being a comment and becomes a thing a test can check. The channel exists because
    // an unknown key is the one thing a rewrite DELETES, and a consumer that means to announce that
    // must be able to name the keys rather than say "something here is not canonical" -- which cannot
    // tell a dropped key apart from a harmless key order or an uppercase GUID.
    std::vector<std::string> warnings;

    [[nodiscard]] bool ok() const;
};

// DOM entry point (the primitive): validate + extract per docs/09 §11. WARNs (unknown keys, at any of
// the three levels) are emitted only when the whole document validates; a rejected document yields
// exactly one error and zero WARNs. Every WARN is ALSO returned, in source order, through
// MaterialParseResult::warnings -- the log line and the returned entry are formatted once, together.
[[nodiscard]] MaterialParseResult parseMaterial(const JsonValue& root);

// Text convenience: parseJson (position-carrying errors) then the DOM overload.
[[nodiscard]] MaterialParseResult parseMaterial(std::string_view text, const JsonParseConfig& config = {});

// The same semantic checks parseMaterial runs, for hand-built documents (the inspector's pre-save
// hook at task 3.4.2). Reaches the arms text cannot: a NaN factor is unspellable in JSON but trivially
// assignable in C++, and every range check is written so NaN fails it. Engaged == invalid.
// writeMaterial does NOT call this (pure emission) — callers wanting the round-trip guarantee run it
// first.
[[nodiscard]] std::optional<MaterialError> validateMaterial(const MaterialDocument& material);

// Emit the document through any writer config. Performs NO validation (unlike scenes there is no
// opaque payload to debug-assert on, so this function has no assert at all). Canonical key order +
// omission rules per docs/09 §11.3.
void writeMaterial(JsonWriter& writer, const MaterialDocument& material);

// Canonical form: default (pretty, 2-space) writer + ONE trailing '\n'. Byte-stable fixpoint over
// valid documents (validateMaterial(m) == nullopt):
//   writeMaterialText(*parseMaterial(writeMaterialText(m)).document) == writeMaterialText(m).
// A non-finite factor has no JSON spelling and would emit as `null`, which parseMaterial then rejects
// — which is exactly why validateMaterial rejects it first rather than letting a file be written that
// cannot be read back.
[[nodiscard]] std::string writeMaterialText(const MaterialDocument& material);

}  // namespace engine

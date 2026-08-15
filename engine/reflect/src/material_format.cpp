// engine/reflect/src/material_format.cpp — task 3.4.1: material asset v1 (docs/09-file-formats.md
// §11). The DOCUMENT layer: strict envelope validation + extraction, plus canonical emission.
// Everything is ITERATIVE (misc-no-recursion is live, and the document is only three levels deep, so
// no recursion is even tempting); nothing asserts on input content (untrusted data — the 1.2.2 D16
// lineage); no exceptions cross the API; no file is ever opened here. Every message is ASCII-only and
// deterministic, and no hash container appears at all — the two key sets are small ordered arrays,
// scanned linearly, so nothing can reach the output in an unspecified order.
#include <aero/core/log.hpp>
#include <aero/reflect/material_format.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine {

namespace {

// ---- the file's vocabulary: root keys, slot keys, and the slot table -------------------------------

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view NAME_KEY = "name";
constexpr std::string_view BASE_COLOR_FACTOR_KEY = "baseColorFactor";
constexpr std::string_view METALLIC_FACTOR_KEY = "metallicFactor";
constexpr std::string_view ROUGHNESS_FACTOR_KEY = "roughnessFactor";
constexpr std::string_view EMISSIVE_FACTOR_KEY = "emissiveFactor";
constexpr std::string_view NORMAL_SCALE_KEY = "normalScale";
constexpr std::string_view OCCLUSION_STRENGTH_KEY = "occlusionStrength";
constexpr std::string_view ALPHA_MODE_KEY = "alphaMode";
constexpr std::string_view ALPHA_CUTOFF_KEY = "alphaCutoff";
constexpr std::string_view DOUBLE_SIDED_KEY = "doubleSided";
constexpr std::string_view TEXTURES_KEY = "textures";

constexpr std::string_view GUID_KEY = "guid";
constexpr std::string_view UV_SET_KEY = "uvSet";
constexpr std::string_view WRAP_U_KEY = "wrapU";
constexpr std::string_view WRAP_V_KEY = "wrapV";
constexpr std::string_view MIN_FILTER_KEY = "minFilter";
constexpr std::string_view MAG_FILTER_KEY = "magFilter";
constexpr std::string_view MIP_FILTER_KEY = "mipFilter";

// Canonical ROOT key order (docs/09 §11.3), and simultaneously the known-key set the WARN sweep tests
// against: one array, so a key added to the schema cannot be emitted and still WARN, or vice versa.
constexpr std::array ROOT_KEYS{
    VERSION_KEY,         NAME_KEY,         BASE_COLOR_FACTOR_KEY,  METALLIC_FACTOR_KEY, ROUGHNESS_FACTOR_KEY,
    EMISSIVE_FACTOR_KEY, NORMAL_SCALE_KEY, OCCLUSION_STRENGTH_KEY, ALPHA_MODE_KEY,      ALPHA_CUTOFF_KEY,
    DOUBLE_SIDED_KEY,    TEXTURES_KEY};

// Canonical SLOT key order, same double duty.
constexpr std::array SLOT_KEYS{GUID_KEY,       UV_SET_KEY,     WRAP_U_KEY,    WRAP_V_KEY,
                               MIN_FILTER_KEY, MAG_FILTER_KEY, MIP_FILTER_KEY};

// The five glTF slots in D7's binding order — the ONE place that order lives. Parse, canonical write
// and the WARN sweep all walk this table, so they cannot disagree about which slots exist or in what
// order they are emitted.
struct SlotBinding {
    std::string_view key;
    std::optional<MaterialTextureSlot> MaterialDocument::*member;
};

constexpr std::array SLOT_BINDINGS{
    SlotBinding{.key = "baseColor", .member = &MaterialDocument::baseColor},
    SlotBinding{.key = "metallicRoughness", .member = &MaterialDocument::metallicRoughness},
    SlotBinding{.key = "normal", .member = &MaterialDocument::normal},
    SlotBinding{.key = "occlusion", .member = &MaterialDocument::occlusion},
    SlotBinding{.key = "emissive", .member = &MaterialDocument::emissive},
};

// The token vocabularies, in canonical enumerator order. The PARSER walks these and compares against
// the very same material*Label functions the WRITER emits, so the file, the logs and the error
// messages share one vocabulary by construction (a label edit is a format change, not a cosmetic).
constexpr std::array ALPHA_MODES{MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask, MaterialAlphaMode::Blend};
constexpr std::array WRAPS{MaterialWrap::Repeat, MaterialWrap::Clamp, MaterialWrap::Mirror};
constexpr std::array FILTERS{MaterialFilter::Nearest, MaterialFilter::Linear};
constexpr std::array MIP_FILTERS{MaterialMipFilter::None, MaterialMipFilter::Nearest, MaterialMipFilter::Linear};

// ---- error construction and the shared catalog lines ------------------------------------------------

// A material-stage error: line/column/offset stay ZERO by contract — the context lives in the message
// text, as a dotted key path ("textures.normal.wrapU"). Only the TEXT overload's JSON-stage failures
// carry positions.
MaterialError materialError(std::string message) {
    MaterialError error;
    error.message = std::move(message);
    return error;
}

// Shortest round-trip, locale-independent — the identical rendering JsonWriter::value(float) produces,
// so an error message and the file agree on how a number is spelled. NaN renders as "nan", which is
// unspellable in JSON and therefore only ever reachable from a hand-built document.
std::string floatText(float value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

// "(found <kind>)" — or, when a Number failed a FORM rule rather than a KIND rule, the quoted lexeme
// instead (the scene_format D11 shape): `(found "1.5")`, `(found "-1")`, `(found string)`.
std::string foundDetail(const JsonValue& value) {
    if (value.isNumber()) {
        return std::format("\"{}\"", value.numberLexeme());
    }
    return std::string(jsonKindName(value.kind()));
}

// One home per shared catalog line, so the parse path and validateMaterial can never diverge.

std::string nilGuidMessage(std::string_view slotPath) {
    return std::format(R"("{}.{}" must not be nil (absence is spelled by omitting "{}"))", slotPath, GUID_KEY,
                       slotPath);
}

std::string uvSetMessage(std::string_view slotPath, std::string_view found) {
    return std::format("\"{}.{}\" must be an integer in [0, {}] (found {})", slotPath, UV_SET_KEY,
                       MATERIAL_MAX_UV_SETS - 1, found);
}

// ---- ranges, written so NaN cannot slip through -----------------------------------------------------

struct NumberRange {
    float minValue;
    float maxValue;
    std::string_view text;  // the message fragment: "in [0, 1]" / ">= 0"
};

// The finite max rather than an infinity: "unbounded above" in the sense docs/09 §11.1 means it
// (HDR-legal emissive) is "every finite float passes". An infinite input is refused one step earlier
// anyway — JSON cannot spell one, and asF32 returns nullopt for a lexeme that rounds to +/-inf, so
// nothing reaches this comparison with an infinity except a hand-built document, which REJECTs here.
constexpr NumberRange UNIT_RANGE{.minValue = 0.0F, .maxValue = 1.0F, .text = "in [0, 1]"};
constexpr NumberRange NON_NEGATIVE_RANGE{
    .minValue = 0.0F, .maxValue = std::numeric_limits<float>::max(), .text = ">= 0"};

// The comparison is deliberately written as !(v >= min && v <= max) rather than (v < min || v > max)
// SO THAT NaN REJECTS: every comparison against NaN is false, so the negation is true. Text can never
// produce a NaN (JSON has no such literal), but a hand-built MaterialDocument trivially can, and
// validateMaterial runs this same function over one.
std::optional<MaterialError> checkRange(float value, std::string_view path, const NumberRange& range) {
    if (value >= range.minValue && value <= range.maxValue) {
        return std::nullopt;
    }
    return materialError(std::format("\"{}\" must be {} (found {})", path, range.text, floatText(value)));
}

// ---- typed readers, all optional-key-tolerant (an absent key leaves the caller's default) ------------

std::optional<MaterialError> readNumber(const JsonValue& value, std::string_view path, const NumberRange& range,
                                        float& out) {
    if (!value.isNumber()) {
        // REJECTing `null` here is deliberate, and stricter than the scene reader's null -> NaN payload
        // tolerance: a material factor has no meaningful "unknown" state (docs/09 §11.1).
        return materialError(std::format("\"{}\" must be a number (found {})", path, jsonKindName(value.kind())));
    }
    // nullopt <=> the lexeme rounds to +/-infinity (docs/09 §2.4's `1e999` corner). The JSON layer keeps
    // number lexemes VERBATIM, so the overflow surfaces here at the typed read — it is not pre-collapsed
    // to `null` before this layer ever sees it.
    const std::optional<float> parsed = value.asF32();
    if (!parsed.has_value()) {
        return materialError(
            std::format(R"("{}" is not representable as a 32-bit float (found "{}"))", path, value.numberLexeme()));
    }
    if (std::optional<MaterialError> error = checkRange(*parsed, path, range)) {
        return error;
    }
    out = *parsed;
    return std::nullopt;
}

std::optional<MaterialError> readOptionalNumber(const JsonValue& parent, std::string_view key, const NumberRange& range,
                                                float& out) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return readNumber(*value, key, range, out);
}

// A fixed-length array of numbers (baseColorFactor: 4, emissiveFactor: 3), glTF's own spelling. The
// caller commits the result only on success, so a rejected document leaves its target untouched.
std::optional<MaterialError> readFactorArray(const JsonValue& value, std::string_view path, const NumberRange& range,
                                             std::span<float> out) {
    if (!value.isArray()) {
        return materialError(std::format("\"{}\" must be an array of {} numbers (found {})", path, out.size(),
                                         jsonKindName(value.kind())));
    }
    const std::vector<JsonValue>& elements = value.elements();
    if (elements.size() != out.size()) {
        return materialError(
            std::format("\"{}\" must be an array of {} numbers (found {})", path, out.size(), elements.size()));
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (std::optional<MaterialError> error =
                readNumber(elements[i], std::format("{}[{}]", path, i), range, out[i])) {
            return error;
        }
    }
    return std::nullopt;
}

std::optional<MaterialError> readOptionalBool(const JsonValue& parent, std::string_view key, bool& out) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    const std::optional<bool> parsed = value->asBool();
    if (!parsed.has_value()) {
        return materialError(std::format("\"{}\" must be a boolean (found {})", key, jsonKindName(value->kind())));
    }
    out = *parsed;
    return std::nullopt;
}

template <typename Enum, std::size_t N, typename LabelFn>
std::string tokenList(const std::array<Enum, N>& values, LabelFn label) {
    std::string list;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            list += ", ";
        }
        list += '"';
        list += label(values[i]);
        list += '"';
    }
    return list;
}

template <typename Enum, std::size_t N, typename LabelFn>
std::optional<MaterialError> readOptionalToken(const JsonValue& parent, std::string_view key, std::string_view path,
                                               const std::array<Enum, N>& values, LabelFn label, Enum& out) {
    const JsonValue* value = parent.find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string_view> text = value->asString();
    if (!text.has_value()) {
        return materialError(std::format("\"{}\" must be a string (found {})", path, jsonKindName(value->kind())));
    }
    for (const Enum candidate : values) {
        if (label(candidate) == *text) {
            out = candidate;
            return std::nullopt;
        }
    }
    return materialError(
        std::format(R"("{}": unknown token "{}" (expected one of {}))", path, *text, tokenList(values, label)));
}

// ---- one texture slot -------------------------------------------------------------------------------

std::optional<MaterialError> parseSlot(const JsonValue& value, std::string_view path, MaterialTextureSlot& out) {
    if (!value.isObject()) {
        return materialError(std::format("\"{}\" must be an object (found {})", path, jsonKindName(value.kind())));
    }

    const JsonValue* guid = value.find(GUID_KEY);
    if (guid == nullptr) {
        return materialError(std::format(R"("{}": missing required key "{}")", path, GUID_KEY));
    }
    const std::string guidPath = std::format("{}.{}", path, GUID_KEY);
    const std::optional<std::string_view> guidText = guid->asString();
    if (!guidText.has_value()) {
        return materialError(std::format("\"{}\" must be a string (found {})", guidPath, jsonKindName(guid->kind())));
    }
    // parseGuid IS the canonical codec: exactly 32 hex digits, any case, and nothing else — so a dashed,
    // braced, short or long value is a hard error, while an UPPERCASE one reads fine and re-emits
    // lowercase. That is docs/09 §5's tolerant-read / canonical-write rule, shared with every other
    // GUID-bearing format in this document; it is not a laxity this format invented.
    const std::optional<Guid> parsedGuid = parseGuid(*guidText);
    if (!parsedGuid.has_value()) {
        return materialError(
            std::format(R"("{}" must be exactly {} hex digits (found "{}"))", guidPath, GUID_TEXT_LENGTH, *guidText));
    }
    if (!parsedGuid->valid()) {
        return materialError(nilGuidMessage(path));
    }
    out.guid = *parsedGuid;

    if (const JsonValue* uvSet = value.find(UV_SET_KEY); uvSet != nullptr) {
        // asU64 already rejects non-Number kinds, non-integral forms and negatives, so one message
        // covers every way the key can be wrong.
        const std::optional<std::uint64_t> raw = uvSet->asU64();
        if (!raw.has_value() || *raw >= MATERIAL_MAX_UV_SETS) {
            return materialError(uvSetMessage(path, foundDetail(*uvSet)));
        }
        out.uvSet = static_cast<std::uint32_t>(*raw);
    }

    const std::string wrapUPath = std::format("{}.{}", path, WRAP_U_KEY);
    if (std::optional<MaterialError> error =
            readOptionalToken(value, WRAP_U_KEY, wrapUPath, WRAPS, materialWrapLabel, out.wrapU)) {
        return error;
    }
    const std::string wrapVPath = std::format("{}.{}", path, WRAP_V_KEY);
    if (std::optional<MaterialError> error =
            readOptionalToken(value, WRAP_V_KEY, wrapVPath, WRAPS, materialWrapLabel, out.wrapV)) {
        return error;
    }
    const std::string minFilterPath = std::format("{}.{}", path, MIN_FILTER_KEY);
    if (std::optional<MaterialError> error =
            readOptionalToken(value, MIN_FILTER_KEY, minFilterPath, FILTERS, materialFilterLabel, out.minFilter)) {
        return error;
    }
    const std::string magFilterPath = std::format("{}.{}", path, MAG_FILTER_KEY);
    if (std::optional<MaterialError> error =
            readOptionalToken(value, MAG_FILTER_KEY, magFilterPath, FILTERS, materialFilterLabel, out.magFilter)) {
        return error;
    }
    const std::string mipFilterPath = std::format("{}.{}", path, MIP_FILTER_KEY);
    return readOptionalToken(value, MIP_FILTER_KEY, mipFilterPath, MIP_FILTERS, materialMipFilterLabel, out.mipFilter);
}

// ---- the envelope: version first, then every scalar, then the slots ----------------------------------

std::optional<MaterialError> extract(const JsonValue& root, MaterialDocument& out) {
    if (!root.isObject()) {
        return materialError(std::format("material root must be a JSON object (found {})", jsonKindName(root.kind())));
    }

    // "version" FIRST (the scene_format D5 precedent): a future-format file must fail with the RIGHT
    // message even when everything after it is garbage.
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return materialError(std::format("missing required key \"{}\"", VERSION_KEY));
    }
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value()) {
        return materialError(std::format("\"{}\" must be an integer (found {})", VERSION_KEY, foundDetail(*version)));
    }
    if (*versionValue != MATERIAL_FORMAT_VERSION) {
        return materialError(std::format("unsupported material format version {} (this build reads version {})",
                                         *versionValue, MATERIAL_FORMAT_VERSION));
    }

    if (const JsonValue* name = root.find(NAME_KEY); name != nullptr) {
        const std::optional<std::string_view> text = name->asString();
        if (!text.has_value()) {
            return materialError(
                std::format("\"{}\" must be a string (found {})", NAME_KEY, jsonKindName(name->kind())));
        }
        out.name = std::string(*text);
    }

    if (const JsonValue* value = root.find(BASE_COLOR_FACTOR_KEY); value != nullptr) {
        std::array<float, 4> rgba{out.baseColorFactor.x, out.baseColorFactor.y, out.baseColorFactor.z,
                                  out.baseColorFactor.w};
        if (std::optional<MaterialError> error = readFactorArray(*value, BASE_COLOR_FACTOR_KEY, UNIT_RANGE, rgba)) {
            return error;
        }
        out.baseColorFactor = Vec4{.x = rgba[0], .y = rgba[1], .z = rgba[2], .w = rgba[3]};
    }
    if (std::optional<MaterialError> error =
            readOptionalNumber(root, METALLIC_FACTOR_KEY, UNIT_RANGE, out.metallicFactor)) {
        return error;
    }
    if (std::optional<MaterialError> error =
            readOptionalNumber(root, ROUGHNESS_FACTOR_KEY, UNIT_RANGE, out.roughnessFactor)) {
        return error;
    }
    if (const JsonValue* value = root.find(EMISSIVE_FACTOR_KEY); value != nullptr) {
        std::array<float, 3> rgb{out.emissiveFactor.x, out.emissiveFactor.y, out.emissiveFactor.z};
        if (std::optional<MaterialError> error =
                readFactorArray(*value, EMISSIVE_FACTOR_KEY, NON_NEGATIVE_RANGE, rgb)) {
            return error;
        }
        out.emissiveFactor = Vec3{.x = rgb[0], .y = rgb[1], .z = rgb[2]};
    }
    if (std::optional<MaterialError> error =
            readOptionalNumber(root, NORMAL_SCALE_KEY, NON_NEGATIVE_RANGE, out.normalScale)) {
        return error;
    }
    if (std::optional<MaterialError> error =
            readOptionalNumber(root, OCCLUSION_STRENGTH_KEY, UNIT_RANGE, out.occlusionStrength)) {
        return error;
    }
    if (std::optional<MaterialError> error = readOptionalToken(root, ALPHA_MODE_KEY, ALPHA_MODE_KEY, ALPHA_MODES,
                                                               materialAlphaModeLabel, out.alphaMode)) {
        return error;
    }
    if (std::optional<MaterialError> error = readOptionalNumber(root, ALPHA_CUTOFF_KEY, UNIT_RANGE, out.alphaCutoff)) {
        return error;
    }
    if (std::optional<MaterialError> error = readOptionalBool(root, DOUBLE_SIDED_KEY, out.doubleSided)) {
        return error;
    }

    const JsonValue* textures = root.find(TEXTURES_KEY);
    if (textures == nullptr) {
        return std::nullopt;
    }
    if (!textures->isObject()) {
        return materialError(
            std::format("\"{}\" must be an object (found {})", TEXTURES_KEY, jsonKindName(textures->kind())));
    }
    // Slot order here is the CANONICAL order, not file order, so the first error a malformed file
    // reports is deterministic regardless of how its keys were typed.
    for (const SlotBinding& binding : SLOT_BINDINGS) {
        const JsonValue* slot = textures->find(binding.key);
        if (slot == nullptr) {
            continue;
        }
        const std::string path = std::format("{}.{}", TEXTURES_KEY, binding.key);
        MaterialTextureSlot parsed;
        if (std::optional<MaterialError> error = parseSlot(*slot, path, parsed)) {
            return error;
        }
        out.*(binding.member) = parsed;
    }
    return std::nullopt;
}

// ---- success-only WARN sweep ------------------------------------------------------------------------

bool isKnownKey(std::string_view key, std::span<const std::string_view> known) {
    for (const std::string_view candidate : known) {
        if (candidate == key) {
            return true;
        }
    }
    return false;
}

// Success path only: a rejected document emits exactly one error and ZERO warns. Document order at
// every level, so the warning stream is deterministic. Re-walks the DOM rather than threading a
// collected list through the readers — the scene_format:264-285 shape, and the one that cannot drift
// out of sync with what extract() actually consumed, because both read the same two key arrays.
void warnUnknownMaterialKeys(const JsonValue& root) {
    for (const JsonMember& member : root.members()) {
        if (!isKnownKey(member.key, ROOT_KEYS)) {
            AERO_LOG_WARN("material: ignoring unknown key \"{}\"", member.key);
        }
    }
    const JsonValue* textures = root.find(TEXTURES_KEY);
    if (textures == nullptr) {
        return;
    }
    for (const JsonMember& member : textures->members()) {
        bool isSlot = false;
        for (const SlotBinding& binding : SLOT_BINDINGS) {
            if (binding.key == member.key) {
                isSlot = true;
                break;
            }
        }
        if (!isSlot) {
            AERO_LOG_WARN("material: \"{}\": ignoring unknown key \"{}\"", TEXTURES_KEY, member.key);
            continue;
        }
        for (const JsonMember& slotMember : member.value.members()) {
            if (!isKnownKey(slotMember.key, SLOT_KEYS)) {
                AERO_LOG_WARN("material: \"{}.{}\": ignoring unknown key \"{}\"", TEXTURES_KEY, member.key,
                              slotMember.key);
            }
        }
    }
}

// ---- canonical emission -----------------------------------------------------------------------------

void writeSlot(JsonWriter& writer, const MaterialTextureSlot& slot) {
    writer.beginObject();
    // All seven sub-keys, always: a BOUND slot spells its sampler state in full (docs/09 §11.3), so a
    // material file reads as its own documentation and no consumer has to remember the defaults.
    const std::string guidText = formatGuid(slot.guid);
    writer.key(GUID_KEY);
    writer.value(std::string_view(guidText));
    writer.key(UV_SET_KEY);
    writer.value(static_cast<unsigned long long>(slot.uvSet));  // explicit cast: the scene_format N1 note
    writer.key(WRAP_U_KEY);
    writer.value(materialWrapLabel(slot.wrapU));
    writer.key(WRAP_V_KEY);
    writer.value(materialWrapLabel(slot.wrapV));
    writer.key(MIN_FILTER_KEY);
    writer.value(materialFilterLabel(slot.minFilter));
    writer.key(MAG_FILTER_KEY);
    writer.value(materialFilterLabel(slot.magFilter));
    writer.key(MIP_FILTER_KEY);
    writer.value(materialMipFilterLabel(slot.mipFilter));
    writer.endObject();
}

}  // namespace

// ---- the public surface -------------------------------------------------------------------------------

std::string_view materialAlphaModeLabel(MaterialAlphaMode mode) noexcept {
    switch (mode) {
        case MaterialAlphaMode::Opaque:
            return "opaque";
        case MaterialAlphaMode::Mask:
            return "mask";
        case MaterialAlphaMode::Blend:
            return "blend";
    }
    return "opaque";  // unreachable for a valid enumerator; never an assert on data
}

std::string_view materialWrapLabel(MaterialWrap wrap) noexcept {
    switch (wrap) {
        case MaterialWrap::Repeat:
            return "repeat";
        case MaterialWrap::Clamp:
            return "clamp";
        case MaterialWrap::Mirror:
            return "mirror";
    }
    return "repeat";
}

std::string_view materialFilterLabel(MaterialFilter filter) noexcept {
    switch (filter) {
        case MaterialFilter::Nearest:
            return "nearest";
        case MaterialFilter::Linear:
            return "linear";
    }
    return "linear";
}

std::string_view materialMipFilterLabel(MaterialMipFilter filter) noexcept {
    switch (filter) {
        case MaterialMipFilter::None:
            return "none";
        case MaterialMipFilter::Nearest:
            return "nearest";
        case MaterialMipFilter::Linear:
            return "linear";
    }
    return "linear";
}

bool MaterialParseResult::ok() const { return document.has_value(); }

MaterialParseResult parseMaterial(const JsonValue& root) {
    MaterialParseResult result;
    MaterialDocument document;

    if (std::optional<MaterialError> error = extract(root, document)) {
        result.error = std::move(*error);
        return result;  // nothing partial: `document` stays unengaged
    }
    warnUnknownMaterialKeys(root);  // success-only sweep, document order
    result.document = std::move(document);
    return result;
}

MaterialParseResult parseMaterial(std::string_view text, const JsonParseConfig& config) {
    const JsonParseResult parsed = parseJson(text, config);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line ok()
    // to `value`, and it would flag the deref below on the Linux lint lane (the scene_format N2 note).
    if (!parsed.value.has_value()) {
        MaterialParseResult result;
        result.error = MaterialError{.message = parsed.error.message,
                                     .line = parsed.error.line,
                                     .column = parsed.error.column,
                                     .offset = parsed.error.offset};
        return result;  // line > 0 marks a JSON-stage failure
    }
    return parseMaterial(*parsed.value);
}

std::optional<MaterialError> validateMaterial(const MaterialDocument& material) {
    const std::array<float, 4> baseColor{material.baseColorFactor.x, material.baseColorFactor.y,
                                         material.baseColorFactor.z, material.baseColorFactor.w};
    for (std::size_t i = 0; i < baseColor.size(); ++i) {
        if (std::optional<MaterialError> error =
                checkRange(baseColor[i], std::format("{}[{}]", BASE_COLOR_FACTOR_KEY, i), UNIT_RANGE)) {
            return error;
        }
    }
    if (std::optional<MaterialError> error = checkRange(material.metallicFactor, METALLIC_FACTOR_KEY, UNIT_RANGE)) {
        return error;
    }
    if (std::optional<MaterialError> error = checkRange(material.roughnessFactor, ROUGHNESS_FACTOR_KEY, UNIT_RANGE)) {
        return error;
    }
    const std::array<float, 3> emissive{material.emissiveFactor.x, material.emissiveFactor.y,
                                        material.emissiveFactor.z};
    for (std::size_t i = 0; i < emissive.size(); ++i) {
        if (std::optional<MaterialError> error =
                checkRange(emissive[i], std::format("{}[{}]", EMISSIVE_FACTOR_KEY, i), NON_NEGATIVE_RANGE)) {
            return error;
        }
    }
    if (std::optional<MaterialError> error = checkRange(material.normalScale, NORMAL_SCALE_KEY, NON_NEGATIVE_RANGE)) {
        return error;
    }
    if (std::optional<MaterialError> error =
            checkRange(material.occlusionStrength, OCCLUSION_STRENGTH_KEY, UNIT_RANGE)) {
        return error;
    }
    if (std::optional<MaterialError> error = checkRange(material.alphaCutoff, ALPHA_CUTOFF_KEY, UNIT_RANGE)) {
        return error;
    }
    for (const SlotBinding& binding : SLOT_BINDINGS) {
        const std::optional<MaterialTextureSlot>& slot = material.*(binding.member);
        if (!slot.has_value()) {
            continue;
        }
        const std::string path = std::format("{}.{}", TEXTURES_KEY, binding.key);
        if (!slot->guid.valid()) {
            return materialError(nilGuidMessage(path));
        }
        if (slot->uvSet >= MATERIAL_MAX_UV_SETS) {
            return materialError(uvSetMessage(path, std::format("{}", slot->uvSet)));
        }
    }
    return std::nullopt;
}

void writeMaterial(JsonWriter& writer, const MaterialDocument& material) {
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<unsigned long long>(MATERIAL_FORMAT_VERSION));
    if (!material.name.empty()) {  // "" == absent (the one scalar with an omission rule)
        writer.key(NAME_KEY);
        writer.value(std::string_view(material.name));
    }
    // Every other scalar is ALWAYS emitted, even at its default: a material file reads as its own
    // documentation, and a diff of two materials lines up field for field (docs/09 §11.3).
    writer.key(BASE_COLOR_FACTOR_KEY);
    writer.beginArray();
    writer.value(material.baseColorFactor.x);
    writer.value(material.baseColorFactor.y);
    writer.value(material.baseColorFactor.z);
    writer.value(material.baseColorFactor.w);
    writer.endArray();
    writer.key(METALLIC_FACTOR_KEY);
    writer.value(material.metallicFactor);
    writer.key(ROUGHNESS_FACTOR_KEY);
    writer.value(material.roughnessFactor);
    writer.key(EMISSIVE_FACTOR_KEY);
    writer.beginArray();
    writer.value(material.emissiveFactor.x);
    writer.value(material.emissiveFactor.y);
    writer.value(material.emissiveFactor.z);
    writer.endArray();
    writer.key(NORMAL_SCALE_KEY);
    writer.value(material.normalScale);
    writer.key(OCCLUSION_STRENGTH_KEY);
    writer.value(material.occlusionStrength);
    writer.key(ALPHA_MODE_KEY);
    writer.value(materialAlphaModeLabel(material.alphaMode));
    writer.key(ALPHA_CUTOFF_KEY);
    writer.value(material.alphaCutoff);
    writer.key(DOUBLE_SIDED_KEY);
    writer.value(material.doubleSided);

    bool anySlot = false;
    for (const SlotBinding& binding : SLOT_BINDINGS) {
        anySlot = anySlot || (material.*(binding.member)).has_value();
    }
    if (anySlot) {  // "textures" omitted iff no slot is bound
        writer.key(TEXTURES_KEY);
        writer.beginObject();
        for (const SlotBinding& binding : SLOT_BINDINGS) {
            const std::optional<MaterialTextureSlot>& slot = material.*(binding.member);
            if (!slot.has_value()) {  // each slot omitted iff absent
                continue;
            }
            writer.key(binding.key);
            writeSlot(writer, *slot);
        }
        writer.endObject();
    }
    writer.endObject();
}

std::string writeMaterialText(const MaterialDocument& material) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space (docs/09 §1's canonical form)
    writeMaterial(writer, material);
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline
    return text;
}

}  // namespace engine

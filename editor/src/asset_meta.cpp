// Aero Engine — the .meta v1 format's pure half (task 3.1.1). project.cpp's structure, one level
// simpler: a two-key, one-level document, so the unknown-key sweep is a single flat loop and needs
// none of project.cpp's depth-first care for "paths". Uses engine::parseJson / engine::JsonValue /
// engine::JsonWriter (F6) -- the scene_format.cpp / project.cpp precedent, applied a third time. No
// <filesystem>, no SDL, no ImGui, no logging (INV-A3): status is RETURNED, never printed
// (project_files.hpp:15-16's convention). No recursion anywhere -- the document is one level deep.
#include <aero/editor/asset_meta.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view GUID_KEY = "guid";
// task 3.2.1 (D6): the optional importer block's own key names, at both levels of nesting.
constexpr std::string_view IMPORTER_KEY = "importer";
constexpr std::string_view IMPORTER_NAME_KEY = "name";
constexpr std::string_view IMPORTER_SETTINGS_KEY = "settings";
constexpr std::string_view IMPORTER_SCALE_KEY = "scale";
constexpr std::string_view IMPORTER_IMPORT_MATERIALS_KEY = "importMaterials";
constexpr std::string_view IMPORTER_IMPORT_ANIMATIONS_KEY = "importAnimations";
constexpr std::string_view IMPORTER_IMPORT_SKINS_KEY = "importSkins";

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (project_files.cpp:44-46's precedent).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

MetaParseResult rejected(MetaError error, std::string message, std::uint32_t line = 0, std::uint32_t column = 0) {
    MetaParseResult result;
    result.error = error;
    result.message = std::move(message);
    result.line = line;
    result.column = column;
    return result;
}

// "(found <kind>)" -- or, when a Number failed a FORM rule rather than a KIND rule, the quoted
// lexeme instead: `(found "1.5")`. Copied from project.cpp:49-54 / scene_format.cpp:43-49 (docs/09
// §5.4's catalog shape, third application).
std::string foundDetail(const JsonValue& v) {
    if (v.isNumber()) {
        return std::format("\"{}\"", v.numberLexeme());
    }
    return std::string(jsonKindName(v.kind()));
}

// task 3.2.1 (D6): parses an already-confirmed-to-be-an-object "importer" block. Returns "" on
// success, with `out` populated and every key this build does not recognise -- whether directly inside
// the block or inside `settings` -- appended to `nestedUnknown` as a DOTTED PATH (AC-14). Returns a
// non-empty message on the FIRST malformed field, matching this format's "first violation wins"
// convention (§5.1): name -> version -> settings, then settings' four keys in the committed order.
// AC-12/INV-M11: the caller never lets a failure here touch result.error or result.guid.
std::string parseImporterBlock(const JsonValue& block, MetaImporterBlock& out,
                               std::vector<std::string>& nestedUnknown) {
    const JsonValue* const nameField = block.find(IMPORTER_NAME_KEY);
    if (nameField == nullptr) {
        return "missing required key \"importer.name\"";
    }
    const std::optional<std::string_view> nameValue = nameField->asString();
    if (!nameValue.has_value()) {
        return std::format("\"importer.name\" must be a string (found {})", jsonKindName(nameField->kind()));
    }
    const JsonValue* const versionField = block.find(VERSION_KEY);
    if (versionField == nullptr) {
        return "missing required key \"importer.version\"";
    }
    const std::optional<std::uint64_t> versionValue = versionField->asU64();
    if (!versionValue.has_value()) {
        return std::format("\"importer.version\" must be an integer (found {})", foundDetail(*versionField));
    }
    const JsonValue* const settingsField = block.find(IMPORTER_SETTINGS_KEY);
    if (settingsField == nullptr) {
        return "missing required key \"importer.settings\"";
    }
    if (!settingsField->isObject()) {
        return std::format("\"importer.settings\" must be a JSON object (found {})",
                           jsonKindName(settingsField->kind()));
    }
    const JsonValue* const scaleField = settingsField->find(IMPORTER_SCALE_KEY);
    if (scaleField == nullptr) {
        return "missing required key \"importer.settings.scale\"";
    }
    // asF32() returns nullopt EXACTLY when the number rounds to +/-inf -- E14's 1e400 is refused with
    // NO hand-rolled finiteness test. A NaN cannot reach here at all: parseJson rejects the literal.
    const std::optional<float> scaleValue = scaleField->asF32();
    if (!scaleValue.has_value()) {
        return std::format("\"importer.settings.scale\" must be a finite number (found {})", foundDetail(*scaleField));
    }
    const JsonValue* const importMaterialsField = settingsField->find(IMPORTER_IMPORT_MATERIALS_KEY);
    if (importMaterialsField == nullptr) {
        return "missing required key \"importer.settings.importMaterials\"";
    }
    const std::optional<bool> importMaterialsValue = importMaterialsField->asBool();
    if (!importMaterialsValue.has_value()) {
        return std::format("\"importer.settings.importMaterials\" must be a boolean (found {})",
                           jsonKindName(importMaterialsField->kind()));
    }
    const JsonValue* const importAnimationsField = settingsField->find(IMPORTER_IMPORT_ANIMATIONS_KEY);
    if (importAnimationsField == nullptr) {
        return "missing required key \"importer.settings.importAnimations\"";
    }
    const std::optional<bool> importAnimationsValue = importAnimationsField->asBool();
    if (!importAnimationsValue.has_value()) {
        return std::format("\"importer.settings.importAnimations\" must be a boolean (found {})",
                           jsonKindName(importAnimationsField->kind()));
    }
    const JsonValue* const importSkinsField = settingsField->find(IMPORTER_IMPORT_SKINS_KEY);
    if (importSkinsField == nullptr) {
        return "missing required key \"importer.settings.importSkins\"";
    }
    const std::optional<bool> importSkinsValue = importSkinsField->asBool();
    if (!importSkinsValue.has_value()) {
        return std::format("\"importer.settings.importSkins\" must be a boolean (found {})",
                           jsonKindName(importSkinsField->kind()));
    }

    out.name = std::string(*nameValue);
    out.version = static_cast<std::uint32_t>(*versionValue);
    out.settings.scale = *scaleValue;
    out.settings.importMaterials = *importMaterialsValue;
    out.settings.importAnimations = *importAnimationsValue;
    out.settings.importSkins = *importSkinsValue;

    for (const JsonMember& member : block.members()) {
        if (member.key != IMPORTER_NAME_KEY && member.key != VERSION_KEY && member.key != IMPORTER_SETTINGS_KEY) {
            nestedUnknown.push_back("importer." + member.key);
        }
    }
    for (const JsonMember& member : settingsField->members()) {
        if (member.key != IMPORTER_SCALE_KEY && member.key != IMPORTER_IMPORT_MATERIALS_KEY &&
            member.key != IMPORTER_IMPORT_ANIMATIONS_KEY && member.key != IMPORTER_IMPORT_SKINS_KEY) {
            nestedUnknown.push_back("importer.settings." + member.key);
        }
    }
    return "";
}

}  // namespace

bool assetContentHashUsable(const AssetRecord& record) noexcept {
    // metaWriteFailed FIRST, for the reason AssetRecord's own comment gives: phase 8 never assigns a
    // `change` to such a record, so it keeps the default UpToDate and every test on `change` alone
    // reads a failed sidecar write as "up to date" for a file with no sidecar and no cache entry.
    return !record.metaWriteFailed && record.change != ImportChange::Unhashable &&
           record.change != ImportChange::NotHashed;
}

std::string metaFileNameFor(std::string_view assetFileName) {
    std::string out(assetFileName);
    out += ASSET_META_SUFFIX;
    return out;
}

bool isMetaFileName(std::string_view fileName) noexcept {
    if (fileName.size() <= ASSET_META_SUFFIX.size()) {
        return false;  // ".meta" alone is 5 bytes, not > 5 -- it is not a sidecar of anything
    }
    const std::size_t offset = fileName.size() - ASSET_META_SUFFIX.size();
    for (std::size_t i = 0; i < ASSET_META_SUFFIX.size(); ++i) {
        const auto a = static_cast<unsigned char>(fileName[offset + i]);
        const auto b = static_cast<unsigned char>(ASSET_META_SUFFIX[i]);
        if (foldAscii(a) != foldAscii(b)) {
            return false;
        }
    }
    return true;
}

std::string_view assetNameForMeta(std::string_view metaName) noexcept {
    if (!isMetaFileName(metaName)) {
        return {};
    }
    // pointer+size constructor: NOT substr() (specified to throw, which would escape this noexcept
    // function -- bugprone-exception-escape, --warnings-as-errors in CI).
    return std::string_view(metaName.data(), metaName.size() - ASSET_META_SUFFIX.size());
}

bool isScannableAssetName(std::string_view fileName) noexcept {
    if (fileName.empty() || fileName.front() == '.') {  // hidden (project_files.cpp's isHiddenName rule)
        return false;
    }
    if (isMetaFileName(fileName)) {
        return false;
    }
    if (fileName.size() >= ATOMIC_TEMP_SUFFIX.size()) {
        const std::size_t offset = fileName.size() - ATOMIC_TEMP_SUFFIX.size();
        bool isTempFile = true;
        for (std::size_t i = 0; i < ATOMIC_TEMP_SUFFIX.size(); ++i) {
            if (fileName[offset + i] != ATOMIC_TEMP_SUFFIX[i]) {
                isTempFile = false;
                break;
            }
        }
        if (isTempFile) {
            return false;  // a SUFFIX test (E19: "wood.png.meta.aero-tmp" too), never equality
        }
    }
    // Two literal names plus the .aero-tmp suffix above (A15) -- both named by this repo's own
    // .gitignore. ".DS_Store" needs no entry: it is dot-prefixed and the hidden check above already
    // removes it. Exact bytes, never a substring test: a file that merely CONTAINS "Thumbs.db" in its
    // name is scannable.
    if (fileName == "Thumbs.db" || fileName == "desktop.ini") {
        return false;
    }
    return true;
}

bool isWatchableAssetName(std::string_view fileName) noexcept {
    return isScannableAssetName(fileName) || isMetaFileName(fileName);
}

// parseMeta's order is load-bearing: it is what makes AC-13's "version-first" property and every
// "exact first error" assertion true. Written in EXACTLY this order; do not reorder for style.
MetaParseResult parseMeta(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
    // ok() to `value` (project.cpp:151-155's precedent).
    if (!parsed.value.has_value()) {
        return rejected(MetaError::BadJson, parsed.error.message, parsed.error.line, parsed.error.column);
    }
    const JsonValue& root = *parsed.value;
    if (!root.isObject()) {
        return rejected(MetaError::NotAnObject,
                        std::format("asset meta root must be a JSON object (found {})", jsonKindName(root.kind())));
    }

    // "version" FIRST (AC-13) -- a document missing BOTH keys must still report the version error.
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return rejected(MetaError::BadVersion, "missing required key \"version\"");
    }
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value()) {
        return rejected(MetaError::BadVersion,
                        std::format("\"version\" must be an integer (found {})", foundDetail(*version)));
    }
    if (*versionValue != static_cast<std::uint64_t>(ASSET_META_FORMAT_VERSION)) {
        return rejected(MetaError::UnsupportedVersion,
                        std::format("unsupported asset meta format version {} (this build reads version {})",
                                    *versionValue, ASSET_META_FORMAT_VERSION));
    }

    // "guid"
    const JsonValue* guidField = root.find(GUID_KEY);
    if (guidField == nullptr) {
        return rejected(MetaError::MissingGuid, "missing required key \"guid\"");
    }
    const std::optional<std::string_view> guidText = guidField->asString();
    if (!guidText.has_value()) {
        return rejected(MetaError::BadGuidKind,
                        std::format("\"guid\" must be a string (found {})", jsonKindName(guidField->kind())));
    }
    const std::optional<Guid> parsedGuid = parseGuid(*guidText);
    if (!parsedGuid.has_value()) {
        return rejected(MetaError::BadGuidText,
                        std::format(R"("guid" must be 32 hexadecimal digits (found "{}"))", *guidText));
    }
    if (!parsedGuid->valid()) {
        return rejected(MetaError::NilGuid, "\"guid\" must not be the nil GUID");
    }

    MetaParseResult result;
    result.guid = *parsedGuid;

    // ---- task 3.2.1: the OPTIONAL importer block (D6 / AC-11..AC-14) --------------------------------
    // Reached ONLY after `version` and `guid` have both been ACCEPTED, so nothing below can affect the
    // identity. Every failure here sets `importerMessage` and leaves `importer` disengaged; NONE of
    // them touches result.error or result.guid (AC-12/INV-M11).
    bool importerEngaged = false;
    if (const JsonValue* const block = root.find(IMPORTER_KEY); block != nullptr) {
        if (!block->isObject()) {
            result.importerMessage =
                std::format("\"importer\" must be a JSON object (found {})", jsonKindName(block->kind()));
        } else {
            MetaImporterBlock parsedBlock;
            std::vector<std::string> nestedUnknown;
            std::string blockMessage = parseImporterBlock(*block, parsedBlock, nestedUnknown);
            if (blockMessage.empty()) {
                result.importer = std::move(parsedBlock);
                importerEngaged = true;
                for (std::string& key : nestedUnknown) {
                    result.unknownKeys.push_back(std::move(key));
                }
            } else {
                result.importerMessage = std::move(blockMessage);
            }
        }
    }

    // Unknown keys, in DOCUMENT ORDER -- never an error. One flat loop: the document is one level
    // deep, so there is no nested walk to get right (contrast project.cpp's "paths" object).
    // "importer" is excluded ONLY when a VALID block engaged (AC-11) -- a MALFORMED "importer" key
    // (e.g. a bare string) is not a key this build actually understood, so it is reported exactly like
    // any other unrecognised root key, matching this file's behaviour before this task existed.
    for (const JsonMember& member : root.members()) {
        if (member.key != VERSION_KEY && member.key != GUID_KEY && !(member.key == IMPORTER_KEY && importerEngaged)) {
            result.unknownKeys.push_back(member.key);
        }
    }
    return result;
}

std::string writeMetaText(Guid guid) { return writeMetaText(guid, ImportSettings{}); }  // INV-M10

std::string writeMetaText(Guid guid, const ImportSettings& settings, std::string_view importerName,
                          std::uint32_t importerVersion) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09's canonical form. Do NOT
                        // spell the config out; a second spelling is a second truth.
    writer.beginObject();
    writer.key("version");
    writer.value(static_cast<long long>(ASSET_META_FORMAT_VERSION));
    writer.key("guid");
    // Named local FIRST (the 2.6.1 FileDialogHost::projectRoot lesson): formatGuid returns by value,
    // and binding a string_view directly to the temporary inside the call expression is legal here
    // (the temporary lives to the end of the full-expression) but the tree's standing rule is
    // named-local-first regardless.
    const std::string guidText = formatGuid(guid);
    writer.value(std::string_view(guidText));
    // D7: OMITTED ENTIRELY when the settings are the defaults, so a creation still writes EXACTLY
    // today's 65 bytes and tests/fixtures/assets/minimal.meta stays byte-identical (AC-9). THIS ONE
    // LINE is what keeps "a scan writes zero bytes to a fully-described tree" true (3.1.1's D6, the
    // single most important invariant in the asset subsystem).
    if (!(settings == ImportSettings{})) {
        writer.key("importer");
        writer.beginObject();
        writer.key("name");
        writer.value(importerName);
        writer.key("version");
        writer.value(static_cast<long long>(importerVersion));
        writer.key("settings");
        writer.beginObject();
        writer.key("scale");
        writer.value(settings.scale);
        writer.key("importMaterials");
        writer.value(settings.importMaterials);
        writer.key("importAnimations");
        writer.value(settings.importAnimations);
        writer.key("importSkins");
        writer.value(settings.importSkins);
        writer.endObject();
        writer.endObject();
    }
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none; parseJson accepts it)
    return text;
}

AssetPlanResult planAssetMetas(std::vector<AssetPlanEntry> entries, GuidGenerator& generator) {
    // 1. Sort byte-lexicographically FIRST (AC-22, seed S15): std::string's `<` is a byte comparison,
    // never case-folded -- so the result is independent of walk order, of the OS, and of
    // entryOrderLess's case folding, and "Z.png" sorts before "a.png".
    std::sort(entries.begin(), entries.end(),
              [](const AssetPlanEntry& a, const AssetPlanEntry& b) { return a.relativePath < b.relativePath; });

    AssetPlanResult result;
    result.records.reserve(entries.size());
    // 2. First pass: settle each record's OWN identity, independent of every other record.
    for (AssetPlanEntry& entry : entries) {
        AssetRecord record;
        record.relativePath = std::move(entry.relativePath);
        record.importSettings = entry.importSettings;  // task 3.2.1 -- carried straight across,
                                                       // independent of which state this record settles into
        if (entry.metaPresent && !entry.guid.has_value()) {
            record.state = AssetMetaState::Invalid;  // nil GUID, no write (AC-25/27)
        } else if (!entry.metaPresent && entry.reattachedGuid.has_value()) {
            // task 3.1.2 (D13/A14) -- BEFORE the plain "!metaPresent" arm, so a metaless asset that
            // asset_database.cpp's phase 5 matched to an orphaned identity is REATTACHED, never minted
            // a fresh one. Never calls generator.next(): the GUID it takes was already in use. An
            // entry with metaPresent == true is never reached here even if reattachedGuid happens to
            // be engaged (the caller never does this) -- it falls through to the Ok arm below and
            // keeps the SIDECAR's own identity, which is what makes that case decidable rather than
            // undefined.
            record.guid = *entry.reattachedGuid;
            record.state = AssetMetaState::Reattached;
        } else if (!entry.metaPresent) {
            record.guid = generator.next();
            record.state = AssetMetaState::Created;
        } else {
            record.guid = *entry.guid;
            record.state = AssetMetaState::Ok;  // D6: a valid, present sidecar is NEVER rewritten
        }
        result.records.push_back(std::move(record));
    }

    // 3. Second pass: duplicate repair (D9), in SORTED order. The first claimant (Ok, Created OR
    // Reattached -- task 3.1.2, D13 condition 4 makes this unreachable in practice, but the general
    // "skip only if Invalid" rule below already covers it, defence in depth) of a GUID keeps it; every
    // LATER claimant gets a fresh, unclaimed GUID and becomes Repaired -- INCLUDING a Reattached
    // record that loses the claim, exactly like any other state. Created GUIDs are inserted into the
    // SAME claim map, so a pathological seed that mints a colliding value is caught by this one code
    // path rather than a second one. Invalid records are never inserted and never repaired (INV-A7) --
    // their GUID is nil, which is never a legitimate claim.
    std::unordered_map<Guid, std::size_t> claimedBy;
    for (std::size_t i = 0; i < result.records.size(); ++i) {
        AssetRecord& record = result.records[i];
        if (record.state == AssetMetaState::Invalid) {
            continue;
        }
        const auto [iterator, inserted] = claimedBy.try_emplace(record.guid, i);
        (void)iterator;
        if (inserted) {
            continue;
        }
        Guid fresh;
        do {
            fresh = generator.next();
        } while (claimedBy.contains(fresh));
        record.guid = fresh;
        record.state = AssetMetaState::Repaired;
        claimedBy.emplace(fresh, i);
    }

    // 4. Tally from the SETTLED state, in one final pass -- so a record whose state changed twice
    // (Created in step 2, then Repaired in step 3 by a colliding fresh GUID) is counted, and its
    // .meta scheduled for write, EXACTLY once. writeIndices is therefore in ascending order by
    // construction and every invariant (`created + repaired + reattached == writeIndices.size()`,
    // task 3.1.2's A14) holds even under that pathological case.
    for (std::size_t i = 0; i < result.records.size(); ++i) {
        switch (result.records[i].state) {
            case AssetMetaState::Created:
                ++result.created;
                result.writeIndices.push_back(i);
                break;
            case AssetMetaState::Repaired:
                ++result.repaired;
                result.writeIndices.push_back(i);
                break;
            case AssetMetaState::Reattached:
                ++result.reattached;
                result.writeIndices.push_back(i);
                break;
            case AssetMetaState::Invalid:
                ++result.invalid;
                break;
            case AssetMetaState::Ok:
                break;
        }
    }

    return result;
}

}  // namespace engine::editor

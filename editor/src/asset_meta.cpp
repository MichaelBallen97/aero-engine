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

}  // namespace

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
    // Unknown keys, in DOCUMENT ORDER -- never an error. One flat loop: the document is one level
    // deep, so there is no nested walk to get right (contrast project.cpp's "paths" object).
    for (const JsonMember& member : root.members()) {
        if (member.key != VERSION_KEY && member.key != GUID_KEY) {
            result.unknownKeys.push_back(member.key);
        }
    }
    return result;
}

std::string writeMetaText(Guid guid) {
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
        if (entry.metaPresent && !entry.guid.has_value()) {
            record.state = AssetMetaState::Invalid;  // nil GUID, no write (AC-25/27)
        } else if (!entry.metaPresent) {
            record.guid = generator.next();
            record.state = AssetMetaState::Created;
        } else {
            record.guid = *entry.guid;
            record.state = AssetMetaState::Ok;  // D6: a valid, present sidecar is NEVER rewritten
        }
        result.records.push_back(std::move(record));
    }

    // 3. Second pass: duplicate repair (D9), in SORTED order. The first claimant (Ok or Created) of a
    // GUID keeps it; every LATER claimant gets a fresh, unclaimed GUID and becomes Repaired. Created
    // GUIDs are inserted into the SAME claim map, so a pathological seed that mints a colliding value
    // is caught by this one code path rather than a second one. Invalid records are never inserted
    // and never repaired (INV-A7) -- their GUID is nil, which is never a legitimate claim.
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
    // construction and every AC-24 invariant (`created + repaired == writeIndices.size()`) holds
    // even under that pathological case.
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

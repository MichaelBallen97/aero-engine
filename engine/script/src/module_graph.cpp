// Aero Engine -- the module loader's backend-free half (task 4.1.2): the specifier grammar, the generation names,
// the record table, the two limits and every load-error text. No backend header: the rules here are tested byte for
// byte at tier 0 (ML1-ML12), and src/vm_modules.cpp is the sequence of backend calls around them.

#include <aero/core/vfs.hpp>  // normalizeVirtualPath: a module key is the very path a VFS read uses
#include <aero/script/internal/module_graph.hpp>
#include <aero/script/vm.hpp>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace engine::script {

std::string_view moduleStateLabel(ModuleState state) noexcept {
    switch (state) {
        case ModuleState::Unloaded:
            return "Unloaded";
        case ModuleState::Loaded:
            return "Loaded";
        case ModuleState::Evaluating:
            return "Evaluating";
        case ModuleState::Evaluated:
            return "Evaluated";
        case ModuleState::Failed:
            return "Failed";
    }
    return "Unknown";
}

}  // namespace engine::script

namespace engine::script::internal {
namespace {

[[nodiscard]] bool hasReservedCharacter(std::string_view text) {
    return text.find_first_of("?#") != std::string_view::npos;
}

// " imported by '<importer>'" -- or nothing, for an entry, which has no importer.
[[nodiscard]] std::string importedBy(std::string_view importer) {
    return importer.empty() ? std::string{} : std::format(" imported by '{}'", importer);
}

// A backend name is pure ASCII: the backend interns a name as an atom by comparing its raw bytes with the existing
// 8-bit atoms before decoding it as UTF-8 (quickjs.c:3358-3368), and replaces invalid UTF-8 with U+FFFD
// (quickjs.c:4380-4407) -- so two non-ASCII keys could share one atom, and a key that is not UTF-8 could not be read
// back. Every byte >= 0x80, and every '%', is written as '%' and two UPPERCASE hex digits; any other byte is itself.
constexpr std::string_view UPPER_HEX = "0123456789ABCDEF";

[[nodiscard]] std::string percentEncodeKey(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80U || c == '%') {
            out.push_back('%');
            out.push_back(UPPER_HEX[byte >> 4U]);
            out.push_back(UPPER_HEX[byte & 0x0FU]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] int upperHexValue(char c) {
    const std::size_t at = UPPER_HEX.find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// The inverse: nullopt for a '%' not followed by two uppercase hex digits. The caller checks the spelling is canonical
// (re-encoding gives the same text), so "%41" for 'A', lowercase digits or a raw non-ASCII byte is never a second name.
[[nodiscard]] std::optional<std::string> percentDecodeKey(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (text.size() - i < 3) {  // a '%' needs two digits after it
            return std::nullopt;
        }
        const int high = upperHexValue(text[i + 1]);
        const int low = upperHexValue(text[i + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

// "res://" + the normal form of `path`, when `path` is addressable and names something below the root.
[[nodiscard]] std::optional<std::string> canonicalKey(std::string_view path) {
    const std::optional<std::string> normal = normalizeVirtualPath(path);
    if (!normal.has_value() || normal->empty()) {
        return std::nullopt;
    }
    return std::string{MODULE_SCHEME} + *normal;
}

}  // namespace

// ---- the grammar ----------------------------------------------------------------------------------------------

SpecifierResolution resolveModuleSpecifier(std::string_view specifier, std::string_view importer) {
    // 1-2: nothing, or a query or fragment -- before anything reads the specifier as a path.
    if (specifier.empty()) {
        return SpecifierResolution{SpecifierRefusal::Empty, {}};
    }
    if (hasReservedCharacter(specifier)) {
        return SpecifierResolution{SpecifierRefusal::Reserved, {}};
    }
    // 3-5: the candidate path, absolute or joined to the importer's directory; every other shape is refused.
    std::string candidate;
    if (specifier.starts_with(MODULE_SCHEME)) {
        candidate = specifier;
    } else if (specifier.starts_with("./") || specifier.starts_with("../")) {
        const std::optional<std::string> base = importerKey(importer);
        if (!base.has_value()) {
            return SpecifierResolution{SpecifierRefusal::NoModuleImporter, {}};
        }
        // The importer's directory: its key up to and including the last '/' (the scheme's own, at the root).
        candidate = base->substr(0, base->rfind('/') + 1);
        candidate += specifier;
    } else if (specifier.starts_with('/')) {
        return SpecifierResolution{SpecifierRefusal::RootRelative, {}};
    } else if (specifier.find(':') < specifier.find('/')) {
        return SpecifierResolution{SpecifierRefusal::OtherScheme, {}};
    } else {
        return SpecifierResolution{SpecifierRefusal::Bare, {}};
    }
    // 6-7: the VFS's own rule makes the key, so a key and the read it causes can never disagree.
    const std::optional<std::string> normal = normalizeVirtualPath(candidate);
    if (!normal.has_value()) {
        return SpecifierResolution{SpecifierRefusal::NotAddressable, {}};
    }
    if (normal->empty()) {
        return SpecifierResolution{SpecifierRefusal::Root, {}};
    }
    return SpecifierResolution{SpecifierRefusal::None, std::string{MODULE_SCHEME} + *normal};
}

std::optional<std::string> importerKey(std::string_view importer) {
    if (std::optional<ModuleName> decoded = decodeModuleName(importer)) {
        return std::move(decoded->key);
    }
    // A script's file name, read leniently: one evaluated under "res://scripts/boot.js" imports relatively.
    if (!importer.starts_with(MODULE_SCHEME) || hasReservedCharacter(importer)) {
        return std::nullopt;
    }
    return canonicalKey(importer);
}

// ---- the generation names -------------------------------------------------------------------------------------

std::string encodeModuleName(std::string_view key, std::uint32_t generation) {
    assert(generation >= 1U);
    std::string name = percentEncodeKey(key);
    if (generation > 1U) {
        name += std::format("{}{}", GENERATION_MARK, generation);
    }
    return name;
}

std::optional<ModuleName> decodeModuleName(std::string_view name) {
    const std::size_t mark = name.find(GENERATION_MARK);
    const std::string_view encoded = name.substr(0, mark);
    // The name part is the key's ASCII spelling, exactly as encodeModuleName writes it; the key itself must be
    // canonical: addressable, below the root, no '?' or '#', and already in its normal form.
    const std::optional<std::string> decoded = percentDecodeKey(encoded);
    if (!decoded.has_value() || percentEncodeKey(*decoded) != encoded) {
        return std::nullopt;
    }
    const std::string& key = *decoded;
    if (!key.starts_with(MODULE_SCHEME) || hasReservedCharacter(key)) {
        return std::nullopt;
    }
    const std::optional<std::string> canonical = canonicalKey(key);
    if (!canonical.has_value() || *canonical != key) {
        return std::nullopt;
    }
    if (mark == std::string_view::npos) {
        return ModuleName{key, 1};
    }
    // One spelling per generation: decimal digits only, the whole tail, no leading zero, in [2, 4294967295].
    const std::string_view digits = name.substr(mark + GENERATION_MARK.size());
    if (digits.empty() || digits.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const end = digits.data() + digits.size();
    const std::from_chars_result parsed = std::from_chars(digits.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < 2U ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return ModuleName{key, static_cast<std::uint32_t>(value)};
}

// ---- the limits -----------------------------------------------------------------------------------------------

std::size_t maxImportDepth(std::size_t stackLimitBytes) noexcept {
    return std::max<std::size_t>(1, stackLimitBytes / IMPORT_STACK_BYTES_PER_LEVEL);
}

bool sourceWithinCap(std::uint64_t sourceBytes, std::size_t heapLimitBytes) noexcept {
    return sourceBytes <= static_cast<std::uint64_t>(heapLimitBytes);
}

// ---- the load-error texts. Every one is built with std::format, never a '%' format: a '%' in a path is text ----

std::string_view specifierRefusalText(SpecifierRefusal refusal) noexcept {
    switch (refusal) {
        case SpecifierRefusal::None:
            return "";
        case SpecifierRefusal::Empty:
            return "an empty specifier names no module";
        case SpecifierRefusal::Bare:
            return "a bare specifier names no file; use res://, ./ or ../";
        case SpecifierRefusal::RootRelative:
            return "a path starting with '/' is not a res:// path; use res://, ./ or ../";
        case SpecifierRefusal::OtherScheme:
            return "only res:// paths are supported, not a URL or a drive path";
        case SpecifierRefusal::Reserved:
            return "a '?' or a '#' is not supported in a module specifier";
        case SpecifierRefusal::NoModuleImporter:
            return "a relative specifier needs a module under res:// to resolve against";
        case SpecifierRefusal::NotAddressable:
            return "it is not an addressable res:// path (a backslash, a NUL, a ':' after the scheme, or a '..' above "
                   "the root)";
        case SpecifierRefusal::Root:
            return "it names the res:// root, not a file";
    }
    return "";
}

std::string refusedSpecifierMessage(std::string_view specifier, std::string_view importer, SpecifierRefusal refusal) {
    return std::format("cannot resolve '{}'{}: {}", specifier, importedBy(importer), specifierRefusalText(refusal));
}

std::string notFoundMessage(std::string_view key, std::string_view importer) {
    return std::format("cannot load '{}'{}: not found or unreadable through the VFS", key, importedBy(importer));
}

std::string tooLargeMessage(std::string_view key, std::string_view importer, std::uint64_t bytes,
                            std::size_t capBytes) {
    return std::format("cannot load '{}'{}: its {} bytes are over the {}-byte source limit (the VM's heap limit)", key,
                       importedBy(importer), bytes, capBytes);
}

std::string tooDeepMessage(std::string_view key, std::string_view importer, std::size_t maxDepth,
                           std::size_t stackLimitBytes) {
    return std::format(
        "cannot load '{}'{}: the import chain is deeper than {} modules, the most a {}-byte stack limit allows", key,
        importedBy(importer), maxDepth, stackLimitBytes);
}

std::string noModuleSourceMessage(std::string_view key, std::string_view importer) {
    return std::format("cannot load '{}'{}: the VM has no module source (it was created without a VirtualFileSystem)",
                       key, importedBy(importer));
}

std::string alreadyLoadedMessage(std::string_view key, std::string_view importer) {
    return std::format("cannot load '{}'{}: its current instance is already loaded under another name", key,
                       importedBy(importer));
}

std::string unsupportedAttributeMessage(std::string_view attribute, std::string_view module) {
    const std::string where = module.empty() ? std::string{} : std::format(" in '{}'", module);
    return std::format("import attribute '{}'{} is not supported (this engine supports none)", attribute, where);
}

std::string cookImportMessage(std::string_view sourceName, std::string_view specifier) {
    return std::format("cannot cook '{}': it imports '{}', and a cooked script imports nothing (bundle it first)",
                       sourceName, specifier);
}

// ---- the record table -----------------------------------------------------------------------------------------

std::uint32_t ModuleTable::generation(std::string_view key) const {
    const auto found = entries.find(key);
    return found == entries.end() ? 1U : found->second.record.generation;
}

void* ModuleTable::instance(std::string_view key) const {
    const auto found = entries.find(key);
    return found == entries.end() ? nullptr : found->second.instance;
}

void ModuleTable::beginCompile(std::string_view key) {
    auto [entry, created] = entries.try_emplace(std::string{key});
    if (created) {
        entry->second.record.path = key;
    }
    entry->second.staticImports.clear();
    entry->second.dynamicImports.clear();
}

void ModuleTable::markLoaded(std::string_view key, void* instance) {
    const auto found = entries.find(key);
    if (found == entries.end() || instance == nullptr) {
        return;
    }
    found->second.instance = instance;
    found->second.record.state = ModuleState::Loaded;
}

void ModuleTable::addImport(std::string_view importer, std::string_view imported, ImportKind kind) {
    const auto found = entries.find(importer);
    if (found == entries.end()) {
        return;  // a script imports without a record to carry the edge
    }
    switch (kind) {
        case ImportKind::Static:
            found->second.staticImports.emplace(imported);
            break;
        case ImportKind::Dynamic:
            found->second.dynamicImports.emplace(imported);
            break;
    }
}

void ModuleTable::markEvaluating(std::string_view key) {
    const auto found = entries.find(key);
    if (found != entries.end() && found->second.instance != nullptr) {
        found->second.record.state = ModuleState::Evaluating;
    }
}

void ModuleTable::markFailed(std::string_view key) {
    const auto found = entries.find(key);
    if (found != entries.end() && found->second.instance != nullptr) {
        found->second.record.state = ModuleState::Failed;
    }
}

void ModuleTable::markEvaluatedGraph(std::string_view entry) {
    // A worklist, never a recursion: a fulfilled entry guarantees its STATIC graph completed. What it imported
    // dynamically may still be pending, and a record without an instance has nothing that ran.
    std::set<std::string, std::less<>> visited;
    std::vector<std::string> pending{std::string{entry}};
    while (!pending.empty()) {
        const std::string key = std::move(pending.back());
        pending.pop_back();
        const auto found = entries.find(key);
        if (found == entries.end() || found->second.instance == nullptr || !visited.insert(key).second) {
            continue;
        }
        found->second.record.state = ModuleState::Evaluated;
        pending.insert(pending.end(), found->second.staticImports.begin(), found->second.staticImports.end());
    }
}

void ModuleTable::retire(const std::vector<std::string>& created) {
    for (const std::string& key : created) {
        const auto found = entries.find(key);
        if (found != entries.end()) {
            bump(found->second);
        }
    }
}

std::vector<std::string> ModuleTable::invalidate(std::string_view key) {
    if (!entries.contains(key)) {
        return {};
    }
    // The importer closure, to a fixpoint: S only grows and is finite, so cycles terminate.
    std::set<std::string, std::less<>> reached{std::string{key}};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& [path, entry] : entries) {
            if (reached.contains(path)) {
                continue;
            }
            const auto importsReached = [&reached](const std::set<std::string, std::less<>>& imports) {
                return std::ranges::any_of(
                    imports, [&reached](const std::string& imported) { return reached.contains(imported); });
            };
            if (importsReached(entry.staticImports) || importsReached(entry.dynamicImports)) {
                reached.insert(path);
                grew = true;
            }
        }
    }
    for (const std::string& path : reached) {
        const auto found = entries.find(path);
        if (found != entries.end()) {
            bump(found->second);
        }
    }
    return {reached.begin(), reached.end()};
}

std::vector<ModuleRecord> ModuleTable::records() const {
    std::vector<ModuleRecord> out;
    out.reserve(entries.size());
    for (const auto& [path, entry] : entries) {
        ModuleRecord record = entry.record;
        std::ranges::set_union(entry.staticImports, entry.dynamicImports, std::back_inserter(record.imports));
        out.push_back(std::move(record));
    }
    return out;
}

void ModuleTable::bump(Entry& entry) {
    assert(entry.record.generation < std::numeric_limits<std::uint32_t>::max());
    ++entry.record.generation;
    entry.instance = nullptr;
    entry.record.state = ModuleState::Unloaded;
}

}  // namespace engine::script::internal

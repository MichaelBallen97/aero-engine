#pragma once
// Aero Engine -- the module loader's backend-free half (task 4.1.2; internal to engine/script): the specifier
// grammar, the generation names, the record table with its import edges and its invalidation closure, the two
// limits, and the text of every load error. Pure: no backend, no VFS read, no clock, no log. Tested exhaustively at
// tier 0 (tests/script_module_test.cpp, ML1-ML12) through aero::script_internal, the HeapAccounting precedent; the
// glue in src/vm_modules.cpp is a sequence of backend calls around it.

#include <aero/script/vm.hpp>  // ModuleState, ModuleRecord, IMPORT_STACK_BYTES_PER_LEVEL

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script::internal {

inline constexpr std::string_view MODULE_SCHEME = "res://";
inline constexpr std::string_view GENERATION_MARK = "?gen=";

// ---- the grammar ----------------------------------------------------------------------------------------------
enum class SpecifierRefusal : std::uint8_t {
    None = 0,
    Empty,             // ""
    Bare,              // "lodash", "aero", ".", "scripts/a.js" -- names no file
    RootRelative,      // "/scripts/a.js"
    OtherScheme,       // a ':' before the first '/': "file:///a.js", "http://x/a.js", "C:/a.js"
    Reserved,          // a '?' or a '#' anywhere: no query, no fragment, and '?' marks a generation
    NoModuleImporter,  // "./x" or "../x" from code whose name is no res:// path (global code, eval, the entry)
    NotAddressable,    // the VFS normalizer refused the result: a '\', a NUL, a ':' after the scheme, '..' above root
    Root,              // the result is the res:// root itself
};

struct SpecifierResolution {
    SpecifierRefusal refusal = SpecifierRefusal::None;
    std::string key;  // "res://<normalized>" iff refusal == None
};

// `importer` is the name the backend gives the importing code: a module's name (a key, possibly with a generation
// mark), a script's file name, "<input>" for eval or Function code, or "" for evaluateModule's entry.
[[nodiscard]] SpecifierResolution resolveModuleSpecifier(std::string_view specifier, std::string_view importer);

// The key relative specifiers resolve against: a module name's key, or a script name that is itself an addressable
// res:// path with no '?' or '#'. nullopt for anything else.
[[nodiscard]] std::optional<std::string> importerKey(std::string_view importer);

// ---- the generation names -------------------------------------------------------------------------------------
struct ModuleName {
    std::string key;
    std::uint32_t generation = 1;
};
// The name a module has inside the VM: the key's ASCII spelling -- every byte >= 0x80 and every '%' written as '%'
// and two uppercase hex digits, every other byte itself -- and, for generation n >= 2, "?gen=" + n.
[[nodiscard]] std::string encodeModuleName(std::string_view key, std::uint32_t generation);
// nullopt unless `name` is exactly what encodeModuleName writes for a canonical key (addressable, not the root, no '?'
// or '#') -- the escapes canonical, uppercase, and only where the encoder puts them -- optionally followed by "?gen="
// and a decimal in [2, 4294967295] without a leading zero.
[[nodiscard]] std::optional<ModuleName> decodeModuleName(std::string_view name);

// ---- the limits -----------------------------------------------------------------------------------------------
// max(1, stackLimitBytes / IMPORT_STACK_BYTES_PER_LEVEL): 64 at the default limit, 8 at the minimum, 1024 at the max.
[[nodiscard]] std::size_t maxImportDepth(std::size_t stackLimitBytes) noexcept;
// A source may be as large as the heap limit and no larger (D13).
[[nodiscard]] bool sourceWithinCap(std::uint64_t sourceBytes, std::size_t heapLimitBytes) noexcept;

// ---- the load-error texts (D20). "[ imported by '<importer>']" appears only for a non-empty importer -------------
[[nodiscard]] std::string_view specifierRefusalText(SpecifierRefusal refusal) noexcept;  // the rule; "" for None
// "cannot resolve '<specifier>'[ imported by '<importer>']: <rule>"
[[nodiscard]] std::string refusedSpecifierMessage(std::string_view specifier, std::string_view importer,
                                                  SpecifierRefusal refusal);
// "cannot load '<key>'[ imported by '<importer>']: not found or unreadable through the VFS"
[[nodiscard]] std::string notFoundMessage(std::string_view key, std::string_view importer);
// "cannot load '<key>'[ imported by '<importer>']: its <bytes> bytes are over the <cap>-byte source limit (the
// VM's heap limit)"
[[nodiscard]] std::string tooLargeMessage(std::string_view key, std::string_view importer, std::uint64_t bytes,
                                          std::size_t capBytes);
// "cannot load '<key>'[ imported by '<importer>']: the import chain is deeper than <depth> modules, the most a
// <stack>-byte stack limit allows"
[[nodiscard]] std::string tooDeepMessage(std::string_view key, std::string_view importer, std::size_t maxDepth,
                                         std::size_t stackLimitBytes);
// "cannot load '<key>'[ imported by '<importer>']: the VM has no module source (it was created without a
// VirtualFileSystem)"
[[nodiscard]] std::string noModuleSourceMessage(std::string_view key, std::string_view importer);
// "cannot load '<key>'[ imported by '<importer>']: its current instance is already loaded under another name"
[[nodiscard]] std::string alreadyLoadedMessage(std::string_view key, std::string_view importer);
// "cannot load '<key>'[ imported by '<importer>']: its cooked bytecode is refused: <reason>"
[[nodiscard]] std::string cookedScriptRefusedMessage(std::string_view key, std::string_view importer,
                                                     std::string_view reason);
// "cannot cook '<sourceName>': it imports '<specifier>', and a cooked script imports nothing (bundle it first)"
[[nodiscard]] std::string cookImportMessage(std::string_view sourceName, std::string_view specifier);
// "import attribute '<attribute>'[ in '<module>'] is not supported (this engine supports none)"
[[nodiscard]] std::string unsupportedAttributeMessage(std::string_view attribute, std::string_view module);

// ---- the record table -----------------------------------------------------------------------------------------
enum class ImportKind : std::uint8_t { Static, Dynamic };

// Every module path the VM has started to compile. `instance` is an opaque handle the glue gives and gets back; the
// table never dereferences it. A mark on an unknown key, or on a record with no instance, changes nothing.
class ModuleTable {
public:
    [[nodiscard]] std::uint32_t generation(std::string_view key) const;  // 1 for a key never compiled
    [[nodiscard]] void* instance(std::string_view key) const;            // the current generation's, or nullptr
    void beginCompile(std::string_view key);  // creates the record (Unloaded, generation 1); clears its imports
    void markLoaded(std::string_view key, void* instance);
    // Records an edge from a module that HAS a record; an importer without one (a script) records nothing.
    void addImport(std::string_view importer, std::string_view imported, ImportKind kind);
    void markEvaluating(std::string_view key);
    void markFailed(std::string_view key);
    void markEvaluatedGraph(std::string_view entry);  // the entry and its transitive STATIC imports with an instance
    void retire(const std::vector<std::string>& created);  // each: generation + 1, no instance, Unloaded
    [[nodiscard]] std::vector<std::string> invalidate(std::string_view key);  // key + transitive importers, sorted
    [[nodiscard]] std::vector<ModuleRecord> records() const;                  // sorted by path

private:
    struct Entry {
        ModuleRecord record;  // record.imports is assembled by records(), never stored here
        void* instance = nullptr;
        std::set<std::string, std::less<>> staticImports;
        std::set<std::string, std::less<>> dynamicImports;
    };
    void bump(Entry& entry);  // generation + 1 (asserts it does not wrap), instance = nullptr, Unloaded
    std::map<std::string, Entry, std::less<>> entries;
};

}  // namespace engine::script::internal

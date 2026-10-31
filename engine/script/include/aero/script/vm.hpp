#pragma once
// Aero Engine -- the JavaScript VM (task 4.1.1; ADR-007). One Vm = one quickjs-ng runtime + one context,
// created together, used on ONE thread, torn down together. A VM offers the ECMAScript language, the backend's
// core globals and, since task 4.2.1, one frozen global of the engine's own: `engine`, holding a class per bound
// component plus Vec3, Quat and Entity. Nothing else is script-visible -- no file, process or timer access. Since
// task 4.1.2 a VM given a VirtualFileSystem also loads ES modules through it (evaluateModule, and import() from
// any code). Since task 4.3.3 a VM loads a script class (an ES module whose default export is a class) and holds its
// instances behind handles, and a changed file re-creates every live instance from the new class.
//
// The backend is an implementation detail of engine/script/src/ and never crosses this header (project
// rule #3). .github/scripts/check-script-boundary.sh and tests/script_boundary_probe.cpp enforce that.
//
// Errors never cross this API as exceptions: every outcome is a status, and every failure is also logged
// ONCE, at ERROR, in the fixed shape listed above evaluateScript below.

#include <aero/core/handle.hpp>
#include <aero/scene/entity.hpp>  // engine::Entity -- the reason aero::scene is PUBLIC

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine {
class VirtualFileSystem;  // forward-declared: create() borrows one; src/vm_modules.cpp includes <aero/core/vfs.hpp>
class World;              // bindWorld borrows one; src/script_api_entity.cpp includes <aero/scene/world.hpp>
}  // namespace engine

namespace engine::script {

namespace internal {
class ComponentBindingTable;  // <aero/script/internal/component_binding.hpp>
struct VmAccess;              // the test seam that creates a VM over another table
struct BytecodeCook;          // the cook (src/bytecode_cook.cpp): two VMs of its own, reached through the friend below
}  // namespace internal

// ---- limits --------------------------------------------------------------------------------------------
// The JavaScript heap: every byte the backend allocates, header included, is charged against it. There
// is deliberately no "unlimited": 0 is refused rather than read as the backend's own "no limit".
inline constexpr std::size_t DEFAULT_HEAP_LIMIT_BYTES = std::size_t{64} * 1024U * 1024U;  // 64 MiB
// The smallest heap a VM may be given. An empty VM needs about 165 KB (152 585 B Release, 164 969 B Debug, with the
// ten built-in components bound; 134 667 B Debug with none -- the `engine` namespace is built in every VM). That
// includes the three builtins the backend builds lazily on a first read, which create reads once so that no later
// read can fail, so no create runs out of heap at or above this; below it the backend's context-creation
// failure path is not safe to take, so such a limit is refused up front instead of being tried.
inline constexpr std::size_t MIN_HEAP_LIMIT_BYTES = std::size_t{1024} * 1024U;  // 1 MiB

// The C stack a call may use below its entry point. 512 KiB fits inside the SMALLEST main-thread stack of
// the five platforms (1 MiB on Windows and iOS) with room left for the frames above the entry and for the
// one interpreter frame that may overshoot between two checks. The calling thread must have at least
// stackLimitBytes + 256 KiB of free stack -- a default 512 KiB macOS/iOS secondary thread does NOT.
inline constexpr std::size_t DEFAULT_STACK_LIMIT_BYTES = std::size_t{512} * 1024U;
inline constexpr std::size_t MIN_STACK_LIMIT_BYTES = std::size_t{64} * 1024U;
inline constexpr std::size_t MAX_STACK_LIMIT_BYTES = std::size_t{8} * 1024U * 1024U;

// The wall-clock budget of ONE call: compile + run + the job drain + error reporting. A call that is still
// executing JavaScript when it runs out is interrupted with an error the script cannot catch, and ends
// Interrupted even when the backend converts that error (a Promise executor, Promise.try and a thenable job all
// do); no new JavaScript call starts in it after that. There is no "unlimited": the engine must never hang on
// a script.
//
// Two backend limits remain, recorded as R21 in docs/08-risks.md. A loop that throws inside a `using` block and
// catches the result on every iteration can run past its budget without end: the backend wraps an interrupt raised
// at the dispose call into a catchable SuppressedError. And an interrupt that rejects a promise a handler was
// already attached to reaches that handler, in a later call, as an error no catch can take.
inline constexpr std::chrono::milliseconds DEFAULT_CALL_TIME_BUDGET{1000};
inline constexpr std::chrono::milliseconds MIN_CALL_TIME_BUDGET{1};
inline constexpr std::chrono::milliseconds MAX_CALL_TIME_BUDGET{60'000};

// How deep one import chain may be loaded: a chain of modules, each importing the next and none loaded before, may
// be maxImportDepth = stackLimitBytes / IMPORT_STACK_BYTES_PER_LEVEL modules long (64 at the default limit, 8 at the
// minimum, 1024 at the maximum); the next one is a LoadError. The 8 KiB is a budget for the C stack one level takes:
// the backend's seven compile frames and this layer's three. Measured on macOS arm64 at 4.3.2, a level takes 3904 B
// in the Debug build (with ASan) and 1040 B in Release, about 1.1 KB and 0.75 KB of them the backend's. A level past
// the budget lets the backend's own stack check refuse a chain the cap admits; MSVC's Debug build has come closest,
// and its figure is not measured (R33 in docs/08-risks.md).
inline constexpr std::size_t IMPORT_STACK_BYTES_PER_LEVEL = std::size_t{8} * 1024U;

struct VmConfig {
    // Each time the budget interrupts a script, the interrupt's own error (and its backtrace) may take up to 16 KiB
    // past this limit, so a call can end with VmStats::heapBytesInUse above heapLimitBytes. Nothing else may.
    std::size_t heapLimitBytes = DEFAULT_HEAP_LIMIT_BYTES;    // >= MIN_HEAP_LIMIT_BYTES
    std::size_t stackLimitBytes = DEFAULT_STACK_LIMIT_BYTES;  // [MIN_STACK_LIMIT_BYTES, MAX_STACK_LIMIT_BYTES]
    std::chrono::milliseconds callTimeBudget = DEFAULT_CALL_TIME_BUDGET;  // [MIN_, MAX_CALL_TIME_BUDGET]
};

// ---- statuses ------------------------------------------------------------------------------------------
enum class VmCreateStatus : std::uint8_t {
    Ok = 0,
    InvalidHeapLimit,   // heapLimitBytes < MIN_HEAP_LIMIT_BYTES
    InvalidStackLimit,  // outside [MIN_STACK_LIMIT_BYTES, MAX_STACK_LIMIT_BYTES]
    InvalidTimeBudget,  // outside [MIN_CALL_TIME_BUDGET, MAX_CALL_TIME_BUDGET]
    OutOfMemory,        // the system allocator failed (the heap limit alone can no longer starve an empty VM)
};
// Never named toString: doctest's DOCTEST_STRINGIFY finds an unqualified toString by ADL (CLAUDE.md).
[[nodiscard]] std::string_view vmCreateStatusLabel(VmCreateStatus status) noexcept;

enum class EvalStatus : std::uint8_t {
    Ok = 0,
    CompileError,  // a source did not compile, or a module graph did not link (a missing or ambiguous export);
                   // nothing ran
    Exception,     // a value was thrown and not caught -- an Error, any other value, or a stack overflow
    OutOfMemory,   // the heap limit refused an allocation and the failure was not caught. In a compile, a refused
                   // growth decides it whatever error the parser then reported; a refusal the backend tolerates (its
                   // atom and shape tables keeping their size) leaves the compile its own CompileError or LoadError
                   // -- a compile whose lost growth no assignment follows does not return at all: R24 in
                   // docs/08-risks.md.
    Interrupted,   // the call's time budget ran out; the script could not catch it, nor start new calls
    InvalidVm,     // a failed create's Vm, or a moved-from one
    WrongThread,   // called from a thread that is not the VM's; nothing was touched
    LoadError,     // a module of the graph could not be obtained -- a refused specifier or import attribute, not found
                   // or unreadable through the VFS, no module source, over the size or depth limit, a cooked container
                   // refused; nothing ran;
                   // decided before any out-of-memory rule: no load refusal is caused by memory
};
[[nodiscard]] std::string_view evalStatusLabel(EvalStatus status) noexcept;

// The outcome of bindWorld and defineEntityGlobal. Every refusal is also logged once, at ERROR, and changes nothing.
enum class BindStatus : std::uint8_t {
    Ok = 0,
    InvalidVm,              // a failed create's Vm, or a moved-from one
    WrongThread,            // called from a thread that is not the VM's; nothing changed
    UnregisteredComponent,  // bindWorld: the World does not register every component the VM binds
    NoWorld,                // defineEntityGlobal: no World is bound
    DeadEntity,             // defineEntityGlobal: the entity is not alive in the bound World
    InvalidName,            // defineEntityGlobal: not an identifier, "engine", or a global the script made undefinable
    OutOfMemory,            // defineEntityGlobal: the heap limit refused the entity object
};
[[nodiscard]] std::string_view bindStatusLabel(BindStatus status) noexcept;

// What was thrown. For an Error object: its name, message and stack text (the backend's frames, one per
// line, no trailing newline; may be empty). For any other value: name and stack are empty and message
// is String(value), or "<unprintable value>" when that conversion itself throws.
struct ScriptError {
    std::string name;
    std::string message;
    std::string stack;
};

struct EvalResult {
    EvalStatus status = EvalStatus::InvalidVm;  // the FIRST failure of the call (the script before its jobs)
    ScriptError error;       // filled iff status is CompileError, Exception, OutOfMemory, Interrupted or LoadError
    std::string completion;  // String(completion value) iff the script ran to completion -- DIAGNOSTIC text,
                             // not a value bridge; "<unprintable value>" when the conversion throws
};

// ---- script classes and their instances (task 4.3.3) ---------------------------------------------------------
// A script is an ES module whose default export is a class. The VM holds each loaded script's class and every instance
// it makes; the caller holds ScriptInstance handles and never an object or a pointer.
//
// A ScriptInstance is only meaningful to the Vm that made it. Passing one to another Vm is a caller error. It is
// mechanically safe -- the other Vm answers as if the handle were stale -- but semantically undefined, and a
// same-index/same-generation coincidence would silently address that Vm's own instance. Do not do it.
struct ScriptInstanceTag;  // incomplete: a tag only
using ScriptInstance = Handle<ScriptInstanceTag>;

enum class ScriptStatus : std::uint8_t {
    Ok = 0,
    Unchanged,        // loadScript: the file holds the bytes the script's class was compiled from; nothing ran
    InvalidVm,        // a failed create's Vm, or a moved-from one
    WrongThread,      // called from a thread that is not the VM's; nothing was touched
    LoadError,        // as EvalStatus: an unresolvable path, no VFS, not found, too large, a refused container
    CompileError,     // as EvalStatus (a link error included)
    Exception,        // the module threw, a constructor threw, or a job of the call threw
    OutOfMemory,      // the heap limit refused an allocation
    Interrupted,      // the call's time budget ran out
    StillEvaluating,  // loadScript: the module's top-level await had not settled when the call ended
    NotAClass,        // the default export is not a class, or a constructor returned a Proxy
    NotLoaded,        // createInstance: no class is loaded from that path
    UnknownInstance,  // destroyInstance: the handle names no live instance (null, destroyed or stale)
    AlreadyRefused,   // loadScript: the file holds the bytes of the script's last refused load since its last
                      // success; nothing ran, nothing is logged, the result's error is empty
};
// Never named toString: doctest's DOCTEST_STRINGIFY finds an unqualified toString by ADL (CLAUDE.md).
[[nodiscard]] std::string_view scriptStatusLabel(ScriptStatus status) noexcept;

struct ScriptLoadResult {
    ScriptStatus status = ScriptStatus::InvalidVm;
    ScriptError error;               // filled iff LoadError, CompileError, Exception, OutOfMemory, Interrupted,
                                     // StillEvaluating or NotAClass
    std::size_t instancesMoved = 0;  // Ok: the live instances now on the new class
    std::size_t fieldsKept = 0;      // Ok: summed over the moved instances, the fields that took the running value
    std::size_t fieldsReset = 0;     // Ok: fields in both versions whose running value did not carry
    std::size_t fieldsAdded = 0;     // Ok: fields only the new class has; they keep their defaults
    std::size_t fieldsDropped = 0;   // Ok: fields only the running instance had, or that stopped being fields
    std::size_t heapBytesInUse = 0;  // VmStats::heapBytesInUse when the call returned; 0 for InvalidVm, WrongThread
    std::size_t heapLimitBytes = 0;  // the VM's limit; 0 for InvalidVm, WrongThread
};

struct ScriptInstanceResult {
    ScriptStatus status = ScriptStatus::InvalidVm;
    ScriptInstance instance;  // valid() iff Ok
    ScriptError error;        // filled iff Exception, OutOfMemory, Interrupted or NotAClass
};

// The state of a module path's CURRENT generation, as the VM last observed it. The backend reports an evaluation's
// outcome only for the module evaluated as an entry, so a dependency of an entry that failed or is still pending stays
// Loaded until an entry that imports it completes. The VM looks at the end of every call, so an entry whose top-level
// await settles in a later call reads Evaluated or Failed from the end of that call.
enum class ModuleState : std::uint8_t {
    Unloaded = 0,  // no instance of the current generation: never compiled, invalidated, or retired by a failed load
    Loaded,        // compiled; not known to have been evaluated
    Evaluating,    // evaluated as an entry whose top-level await had not settled when the VM last looked
    Evaluated,     // it, and every module it imports statically, completed
    Failed,        // evaluated as an entry and threw; every later import rethrows its recorded error until invalidated
};
[[nodiscard]] std::string_view moduleStateLabel(ModuleState state) noexcept;

// One entry of the module cache. Inside the VM a module is named by its path's ASCII spelling -- every byte >= 0x80
// and every '%' written as '%' and two uppercase hex digits -- followed, from generation 2, by "?gen=<n>": that is
// what a stack frame shows ("res://scripts/a.js?gen=2:3:5", "res://%C3%BC/b.js:1:10"). The log records and the cache
// keep the canonical path. No specifier can name a later generation: '?' is refused in every specifier and entry path.
struct ModuleRecord {
    std::string path;              // the canonical "res://..." path
    std::uint32_t generation = 1;  // one more per invalidation and per retirement after a failed load or link
    ModuleState state = ModuleState::Unloaded;
    std::vector<std::string> imports;  // paths this generation imported, statically or dynamically; sorted, unique
};

struct VmStats {
    std::size_t heapLimitBytes = 0;
    // Live blocks, each with its 16-byte header. It can exceed heapLimitBytes by up to 16 KiB for each time the
    // budget interrupted a script (see VmConfig::heapLimitBytes).
    std::size_t heapBytesInUse = 0;
    std::size_t heapBytesPeak = 0;
    std::size_t liveAllocations = 0;
    // Requests the heap limit refused, over the VM's life. A bytecode read the headroom guard refused counts as one.
    std::uint64_t refusedAllocations = 0;
    std::uint64_t unhandledRejections = 0;    // reported at the end of a call
    std::uint64_t rejectionsHandledLate = 0;  // reported, then handled in a later call
    // Module instances compiled over the VM's life. None is freed before teardown -- the backend has no way to drop a
    // module -- so every reload of a module adds one (about 1.5 KB for a small module, plus about three times its
    // source; measured at 4.1.2). Recreating the VM is the full reset.
    std::uint64_t moduleInstances = 0;
};

// What teardown found. A leak-free VM reports zeros. NOTE: in a build whose backend has assertions on
// (every *-debug preset), a leaked object or function aborts inside the backend's teardown before this
// report exists; a leaked string is still reported here.
//
// The last two count a backend defect the VM contains (R23 in docs/08-risks.md): when a module's top-level await is
// still pending at teardown and something still refers to that evaluation -- a resolver a global or a module binding
// keeps, an importer awaiting it, an import() of it -- the backend frees the module first and writes into it
// afterwards. The VM keeps every block the backend releases during teardown until the backend is done, so those writes
// land in memory the VM still owns: writtenAfterRelease counts the blocks written after their release (0 unless such
// an await was pending), repeatedReleases the blocks the backend handed back again -- released or reallocated a
// second time -- each absorbed and still freed exactly once (none measured).
struct VmTeardownReport {
    std::size_t leakedAllocations = 0;
    std::size_t leakedBytes = 0;
    std::size_t writtenAfterRelease = 0;
    std::size_t repeatedReleases = 0;
};

struct VmCreateResult;

class Vm {
public:
    // No module source: evaluateModule and every import are refused with a LoadError / a rejection that says so.
    [[nodiscard]] static VmCreateResult create(const VmConfig& config = {});
    // `modules` is BORROWED and must outlive the Vm. Every module is read through it and through nothing else. It may
    // be mounted or cleared between calls on the VM's thread, never while a call is in flight.
    [[nodiscard]] static VmCreateResult create(const VmConfig& config, const VirtualFileSystem& modules);
    static VmCreateResult create(const VmConfig& config, const VirtualFileSystem&& modules) = delete;  // no temporary

    Vm() noexcept;  // an INVALID Vm: what a failed create returns and what a move leaves behind
    Vm(Vm&& other) noexcept;
    Vm& operator=(Vm&& other) noexcept;  // tears the assigned-to VM down first
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    ~Vm();  // == shutdown(), report discarded (its ERROR record is not)

    [[nodiscard]] bool valid() const noexcept;

    // THE entry point. Compiles `source` as strict global code under `fileName` (stack traces and every
    // log record name it), runs it, drains the job queue, reports unhandled promise rejections, and
    // returns. Both strings are copied: neither needs a terminator nor outlives the call.
    //
    // Every failure is logged once, at ERROR, as "script: <fileName>: " followed by:
    //   CompileError   "compile error: <name>: <message>"                       + "\n<stack>"
    //   Exception      "uncaught <name>: <message>"                             + "\n<stack>"  (an Error)
    //                  "uncaught exception: <String(value)>"                                   (anything else)
    //   OutOfMemory    "out of memory (heap limit <L> bytes, <U> bytes in use)" + "\n<stack>"
    //                  ... + ": <name>: <message>" before the stack when the error the backend reported is not its
    //                  own out-of-memory error (a compile the heap starved: the parser's SyntaxError)
    //   Interrupted    "interrupted after exceeding the <B> ms time budget"     + "\n<stack>"
    //                  "... time budget with promise jobs still queued"        (the drain was cut short)
    //   a rejection    "unhandled promise rejection: <name>: <message>"         + "\n<stack>"  (status unchanged)
    // and, at INFO, "a promise rejection reported as unhandled was handled later". A "\n<stack>" tail is
    // present only when the stack text is non-empty. A rejection whose reason is an object other than an Error is
    // described through its own toString; when the call's budget ran out first, that call is refused and the reason
    // reads "<unprintable value>".
    [[nodiscard]] EvalResult evaluateScript(std::string_view source, std::string_view fileName);

    // Loads the module at `virtualPath` -- an ABSOLUTE res:// path, refused otherwise -- and its static import graph
    // through the VFS, links it, evaluates it, drains the job queue and reports unhandled promise rejections, all under
    // ONE call's time budget. A module evaluates once: calling this again for a module that already ran re-runs
    // nothing and returns its recorded outcome (Ok, or its error, logged again). `completion` stays empty: a module
    // has no completion value. Nothing about the module's exports crosses this API.
    //
    // Inside a module, a specifier is "res://...", "./..." or "../..." (relative to the importing module); anything
    // else is refused -- a bare name, a '/'-rooted path, another scheme or a drive path, "", a '?' or a '#'. Paths are
    // compared byte for byte (case-sensitive). No import attribute is supported. import() follows the same rules; from
    // global code only an absolute res:// specifier resolves. A specifier ends at its first U+0000: the backend hands
    // specifiers over as C strings, so res://a.js followed by a U+0000 and anything else names res://a.js.
    //
    // Every failure is logged once, at ERROR, as "script: <path>: " followed by evaluateScript's shapes, plus:
    //   LoadError      "load error: <name>: <message>"                 (no stack: nothing ran)
    //   CompileError   also a link error: "compile error: SyntaxError: Could not find export 'x' in module '...'"
    // and, at WARN, "module evaluation is still pending at the end of the call (a top-level await has not settled)"
    // -- never in a call its budget ended.
    //
    // A call its budget ended records the interrupt once, whatever else failed after it. A module that had already
    // failed is recorded Failed and its own error is reported by its next evaluation; a top-level await the budget
    // interrupted never settles, so its module stays Evaluating -- every later evaluateModule of it is Ok with the
    // pending WARN -- until invalidateModule gives it a new generation.
    //
    // A module's own failure is reported once. During this call it is the call's status and its one record. A
    // top-level await that rejects in a LATER call is reported by that call, as one "unhandled promise rejection"
    // record. A failed import() rejects its promise and logs nothing itself: the script may catch it. An import() whose
    // graph the heap limit starved while it compiled rejects with the backend's out-of-memory error
    // (InternalError: out of memory), the parser's own error as its `cause` when the parser reported one. A rejection a
    // module body makes with the very value the module then fails with, while its evaluation is pending, counts as that
    // one failure.
    //
    // A module file that begins with the cooked script magic (docs/09 section 16) is bytecode `aero_cooker script`
    // wrote; any other file is JavaScript. Both are read, limited and named the same way. A container this engine
    // cannot use is a LoadError before the backend reads it; one the heap has no room to read safely is OutOfMemory
    // (R31).
    [[nodiscard]] EvalResult evaluateModule(std::string_view virtualPath);

    // The module cache: every path the VM has started to compile, sorted by path. The VM's thread only: from another
    // thread it is refused with one ERROR and returns nothing.
    [[nodiscard]] std::vector<ModuleRecord> moduleRecords() const;

    // The reload hook. Gives `virtualPath` and every module that imported it, directly or transitively, statically or
    // dynamically, a new generation: the next load of each reads the VFS again and builds a NEW instance (the old one
    // keeps running wherever it is still referenced). Returns the paths it changed, sorted; an unknown path changes
    // nothing. An unaddressable path, or a call from another thread, is refused with one ERROR and changes nothing.
    std::vector<std::string> invalidateModule(std::string_view virtualPath);

    // Runs the cycle collector now. Reference counting frees everything else immediately.
    void collectGarbage() noexcept;

    // The VM's thread only (a debug assert says so).
    [[nodiscard]] VmStats stats() const noexcept;

    // ---- the World a script's entities live in (task 4.2.1) -----------------------------------------------------
    // The World is BORROWED and must outlive the binding: until unbindWorld, the next bindWorld or shutdown. No call
    // may run while the caller iterates that World. EVERY call starts a new binding, the same World included: an
    // entity object a script made under an earlier binding throws on every access, because an Entity means
    // something only to the World that minted it (entity.hpp). Refused with UnregisteredComponent and one ERROR,
    // the previous binding kept, when the World does not register every component this VM binds (a moved-from
    // World registers none). Success logs nothing. Never from inside a call.
    BindStatus bindWorld(World& world);
    BindStatus bindWorld(World&& world) = delete;  // no temporary

    // Every later access through an entity object throws "no World is bound". Idempotent; an invalid VM or another
    // thread is one ERROR and no change.
    void unbindWorld() noexcept;

    // Defines globalThis[name] as an engine.Entity for `entity`: writable, configurable and enumerable, through a
    // define that runs no script setter; the same name again replaces it. Refusals, each one ERROR and no change, in
    // this order: InvalidVm, WrongThread, InvalidName (the grammar [A-Za-z_$][A-Za-z0-9_$]*, or "engine"), NoWorld,
    // DeadEntity, then OutOfMemory (the object) or InvalidName (the define returned false: a non-configurable global
    // of that name, or a non-extensible global object).
    BindStatus defineEntityGlobal(std::string_view name, Entity entity);

    // ---- script classes and instances (task 4.3.3) ------------------------------------------------------------
    // Loads the script at `virtualPath` (an ABSOLUTE res:// path): the first load and every reload are this one call,
    // so any trigger may call it. A file holding the bytes the script's class was compiled from is Unchanged: nothing
    // is read beyond the comparison, nothing runs, nothing is logged. Any other bytes are ONE call under one time
    // budget: the VM evaluates a new generation of the module, takes its default export, which must be a class, and
    // re-creates every live instance of the script from it. The call commits only if everything succeeded, and only
    // after the call's tail (its job drain and its unhandled-rejection report); a refusal leaves every instance on its
    // old object and the script on its last good class, so later instances come from that class.
    //
    // A script's identity is its canonical path, never the class's `name` (esbuild renames a class on a collision). A
    // reload re-creates: each new instance runs its constructor and field initializers, and its handle points at it.
    // Three consequences are documented: a native `#private` field restarts, a constructor runs again at every reload,
    // and an old object something else still holds keeps the old code and state. The comparison is of the script's own
    // file: a script is one esbuild bundle (4.3.1), so a change to anything it imports changes its bytes.
    //
    // A refused load remembers its bytes. The same bytes again, before the script loads again, are AlreadyRefused:
    // nothing is read beyond the comparison, nothing runs, nothing is logged and the result's error is empty, so a
    // trigger that fires again with a file still broken neither retries nor reports twice. Other bytes are attempted
    // anew, and so are the bytes of a refused load after any success.
    //
    // Success logs nothing, and neither do Unchanged and AlreadyRefused. Every other outcome logs exactly one ERROR,
    // plus whatever unrelated records the call's tail logs. The loader's and the trap's records are evaluateModule's
    // (a LoadError, a CompileError, an Exception, an Interrupted). A load the heap refused (OutOfMemory) is the
    // engine's own, and never carries the backend's error text: "script: <path>: out of memory (heap limit <L> bytes,
    // <U> bytes in use): the script and its instances are unchanged, and only a new VM resets the heap". Every
    // instance keeps its code and values, and only a new VM frees an old generation. The engine's other records, each
    // after "script: <path>: ":
    //   StillEvaluating  "not a script: its top-level await had not settled when the load ended"
    //   NotAClass        "not a script: its default export is <what>, not a class"
    //                    "not a script: its constructor returned a Proxy"
    // and, without a path: "script: loadScript on an invalid Vm (a failed create or a moved-from Vm)" for InvalidVm,
    // and "script: loadScript called from a thread that is not the VM's; refused" for WrongThread.
    //
    // What a reload carries. A field is an own, enumerable, string-keyed DATA property; an accessor is not a field,
    // whoever defined it, and neither is a non-enumerable property. After the new instance is constructed, each field
    // of the running instance that the new instance also has takes the running value when the two KINDS agree
    // (boolean, number, bigint, string, symbol, array, engine.Vec3, engine.Quat, engine.Entity, object); values and
    // defaults are never compared, so a changed default does not reset a field. Two relaxations: a new default of
    // `undefined` (a field declared without a value) takes any kind, and a new default of `null` takes null and every
    // reference kind. A function never carries, on either side: the new instance keeps its own. A carried object is
    // carried by reference and keeps the code it was made with. A field the running instance had and the new one
    // lacks, or that the new class turned into an accessor, is dropped and the accessor takes over; one only the new
    // instance has keeps its default. Fields are read as descriptors and written with a define, so no getter and no
    // setter runs, and a frozen or otherwise unwritable new field keeps its default (counted as reset) rather than
    // refusing the reload. fieldsKept, fieldsReset, fieldsAdded and fieldsDropped sum these over the moved instances; a
    // refused load reports none. A class is a constructor whose own `prototype` is a non-writable data property: a
    // plain, bound, arrow, async or generator function, an object, a primitive and a missing default are not, and
    // finding out runs no script code. The VM's thread only, and never from inside another call.
    [[nodiscard]] ScriptLoadResult loadScript(std::string_view virtualPath);

    // Constructs one instance of the script's current class under one call's time budget. NotLoaded when no class is
    // loaded from that path ("script: <path>: createInstance refused: no script class is loaded from it"); Exception,
    // OutOfMemory and Interrupted when the constructor fails; NotAClass when it returned a Proxy. Each refusal is one
    // ERROR and makes nothing. Each instance holds its own fields. A constructor that returns a Proxy is refused, so no
    // live instance is a Proxy.
    [[nodiscard]] ScriptInstanceResult createInstance(std::string_view virtualPath);

    // Frees an instance. UnknownInstance, with one ERROR ("script: destroyInstance refused: the handle names no live
    // instance"), when the handle is null, already destroyed or stale. Runs no script code.
    ScriptStatus destroyInstance(ScriptInstance instance);  // not [[nodiscard]]: the unbindWorld precedent

    // Frees the context and the runtime and reports what did not come back. Idempotent; leaves the Vm
    // invalid. May run on any thread provided no call is in flight.
    [[nodiscard]] VmTeardownReport shutdown() noexcept;

private:
    struct Impl;  // holds the backend's runtime + context; defined in src/vm_impl.hpp
    explicit Vm(std::unique_ptr<Impl> owned) noexcept;
    friend struct internal::VmAccess;
    friend struct internal::BytecodeCook;
    // `components` is what the `engine` namespace binds: BORROWED, and it outlives the Vm.
    [[nodiscard]] static VmCreateResult createWith(const VmConfig& config, const VirtualFileSystem* modules,
                                                   const internal::ComponentBindingTable& components);
    std::unique_ptr<Impl> impl;  // heap-pinned: the backend's callbacks hold Impl*, never Vm*
};

struct VmCreateResult {
    VmCreateStatus status = VmCreateStatus::OutOfMemory;
    Vm vm;  // valid() iff status == Ok
};

// The backend's version string ("0.15.1" at the pinned vcpkg baseline).
[[nodiscard]] std::string_view scriptBackendVersion() noexcept;

}  // namespace engine::script

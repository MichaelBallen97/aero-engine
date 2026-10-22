// Aero Engine -- the script quickjs-boundary COMPILE-TIME guard (task 4.1.1; project rule #3; risk R12).
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is that it
// COMPILES. Its target (aero_script_boundary_probe, tests/CMakeLists.txt) links ONLY aero::script, which
// links quickjs-ng PRIVATE -- and whose only PUBLIC dependency, aero::core, propagates no vcpkg header -- so
// vcpkg's shared per-triplet include/ root never reaches this compile line and <quickjs.h> is genuinely
// unresolvable here. If a public script header ever includes it, THIS TU fails to compile the moment the
// leak is written. quickjs-ng is the sixth PRIVATE flat-root backend this mechanism bites for, after glm,
// SDL3, miniaudio, SDL_GPU and EnTT.
//
// aero_tests CANNOT do this: it links doctest and SDL3 directly and inherits the whole shared root. The
// textual half -- which also catches a forward declaration, invisible to any compiler -- is
// .github/scripts/check-script-boundary.sh.
//
// KEEP THIS TARGET DEPENDENCY-FREE: .github/scripts/check-boundary-probes.sh refuses any link but exactly one
// aero:: library, PRIVATE, and boundary-probes.probe_compile_line reads this TU's compile line.

#include <aero/core/vfs.hpp>       // task 4.1.2: create() borrows one; it includes only the standard library
#include <aero/script/script.hpp>  // the umbrella

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace script = engine::script;

// ---- limits and defaults ----
static_assert(script::DEFAULT_HEAP_LIMIT_BYTES == std::size_t{64} * 1024U * 1024U);
static_assert(script::DEFAULT_STACK_LIMIT_BYTES == std::size_t{512} * 1024U);
static_assert(script::MIN_HEAP_LIMIT_BYTES <= script::DEFAULT_HEAP_LIMIT_BYTES);
static_assert(script::MIN_STACK_LIMIT_BYTES <= script::DEFAULT_STACK_LIMIT_BYTES);
static_assert(script::DEFAULT_STACK_LIMIT_BYTES <= script::MAX_STACK_LIMIT_BYTES);
static_assert(script::DEFAULT_CALL_TIME_BUDGET == std::chrono::milliseconds{1000});
static_assert(script::MIN_CALL_TIME_BUDGET <= script::DEFAULT_CALL_TIME_BUDGET);
static_assert(script::DEFAULT_CALL_TIME_BUDGET <= script::MAX_CALL_TIME_BUDGET);
static_assert(script::VmConfig{}.heapLimitBytes == script::DEFAULT_HEAP_LIMIT_BYTES);
static_assert(script::VmConfig{}.stackLimitBytes == script::DEFAULT_STACK_LIMIT_BYTES);
static_assert(script::VmConfig{}.callTimeBudget == script::DEFAULT_CALL_TIME_BUDGET);

// ---- the Vm: move-only, invalid by default, statuses with noexcept labels ----
static_assert(static_cast<std::uint8_t>(script::VmCreateStatus::Ok) == 0U);
static_assert(noexcept(script::vmCreateStatusLabel(script::VmCreateStatus::Ok)));
static_assert(!std::is_copy_constructible_v<script::Vm>);
static_assert(!std::is_copy_assignable_v<script::Vm>);
static_assert(std::is_nothrow_move_constructible_v<script::Vm>);
static_assert(std::is_nothrow_move_assignable_v<script::Vm>);
static_assert(std::is_nothrow_default_constructible_v<script::Vm>);
static_assert(std::is_default_constructible_v<script::VmCreateResult>);
static_assert(script::VmStats{}.heapBytesInUse == 0U);
static_assert(script::VmTeardownReport{}.leakedAllocations == 0U);
static_assert(noexcept(script::scriptBackendVersion()));
static_assert(std::is_same_v<decltype(script::scriptBackendVersion()), std::string_view>);

// ---- the entry point ----
static_assert(static_cast<std::uint8_t>(script::EvalStatus::Ok) == 0U);
static_assert(noexcept(script::evalStatusLabel(script::EvalStatus::Ok)));
static_assert(
    std::is_same_v<decltype(std::declval<script::Vm&>().evaluateScript(std::string_view{}, std::string_view{})),
                   script::EvalResult>);
static_assert(std::is_default_constructible_v<script::EvalResult>);

// ---- modules (task 4.1.2) ----
static_assert(static_cast<std::uint8_t>(script::EvalStatus::LoadError) == 7U);  // appended: earlier values unchanged
static_assert(script::IMPORT_STACK_BYTES_PER_LEVEL == std::size_t{8} * 1024U);
static_assert(static_cast<std::uint8_t>(script::ModuleState::Unloaded) == 0U);
static_assert(noexcept(script::moduleStateLabel(script::ModuleState::Loaded)));
// ModuleRecord's default generation is pinned at tier 0 (ML6), never here: a constant expression that constructs and
// destroys its std::string and std::vector members leans on constexpr containers, MSVC's Debug iterator machinery
// included, for nothing a doctest case does not already assert.
static_assert(script::VmStats{}.moduleInstances == 0U);
static_assert(
    std::is_same_v<decltype(std::declval<script::Vm&>().evaluateModule(std::string_view{})), script::EvalResult>);
static_assert(
    std::is_same_v<decltype(std::declval<const script::Vm&>().moduleRecords()), std::vector<script::ModuleRecord>>);
static_assert(std::is_same_v<decltype(std::declval<script::Vm&>().invalidateModule(std::string_view{})),
                             std::vector<std::string>>);
// The VFS is borrowed: an lvalue binds, a temporary does not (the rvalue overload is deleted). A file-local concept,
// because a requires-expression only substitutes when it is dependent (CLAUDE.md, HE17's shape).
template <typename Vfs>
concept CreatesFrom = requires(Vfs&& vfs) { script::Vm::create(script::VmConfig{}, std::forward<Vfs>(vfs)); };
static_assert(CreatesFrom<const engine::VirtualFileSystem&>);  // the positive control
static_assert(!CreatesFrom<engine::VirtualFileSystem>);

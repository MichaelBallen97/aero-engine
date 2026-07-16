// Aero Engine — the platform SDL/miniaudio-boundary COMPILE-TIME guard (task 0.3.1; miniaudio added
// 0.3.3).
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is
// that it COMPILES. Its target (aero_platform_boundary_probe, tests/CMakeLists.txt) links ONLY
// aero::platform, which links SDL3 PRIVATE, so vcpkg's shared per-triplet include/ root never
// reaches this compile line and SDL is genuinely unreachable here. If any public platform header
// starts including <SDL3/...>, THIS TU fails to compile the moment the leak is written.
//
// aero_tests CANNOT do this: it links doctest AND SDL3 and inherits the whole shared vcpkg include
// root, so SDL would resolve there regardless of what aero_platform links (risk R12).
//
// KEEP THIS TARGET DEPENDENCY-FREE — see tests/CMakeLists.txt.

#include <aero/platform/platform.hpp>

#include <cstdint>
#include <type_traits>

// Instantiate the public surface via constexpr-value + type-trait asserts. NO sizeof(T) > 0 /
// == constant — that trips bugprone-sizeof-expression under --warnings-as-errors='*'
// (see tests/jobs_boundary_probe.cpp). No NAMED entity is declared, so docs/04's naming law has
// nothing to bind to.
static_assert(!engine::platform::WindowId{}.valid());
static_assert(engine::platform::WindowId{7}.valid());
static_assert(engine::platform::Event{}.type == engine::platform::EventType::None);
static_assert(static_cast<unsigned>(engine::platform::EventType::None) == 0U);

// Context/Window/*Config are not constant-constructible (std::thread::id / unique_ptr /
// std::string), so force them COMPLETE through traits that ALSO assert a real API contract.
static_assert(!std::is_copy_constructible_v<engine::platform::Context>);  // one-per-process
static_assert(!std::is_move_constructible_v<engine::platform::Context>);
static_assert(!std::is_copy_constructible_v<engine::platform::Window>);  // move-only RAII
static_assert(std::is_move_constructible_v<engine::platform::Window>);
static_assert(std::is_default_constructible_v<engine::platform::WindowConfig>);
static_assert(std::is_default_constructible_v<engine::platform::ContextConfig>);
static_assert(std::is_default_constructible_v<engine::platform::WindowSize>);

// ---- task 0.3.2 input surface ----
static_assert(engine::platform::Key::A != engine::platform::Key::B);
static_assert(static_cast<int>(engine::platform::MouseButton::Left) == 0);
static_assert(engine::platform::KeyMods{}.bits == 0);
static_assert(!engine::platform::KeyMods{}.shift());
static_assert(engine::platform::KeyMods{static_cast<std::uint16_t>(engine::platform::KeyMod::LeftCtrl)}.ctrl());
static_assert(engine::platform::MousePosition{}.x == 0.0f);
static_assert(engine::platform::Event{}.type == engine::platform::EventType::None);  // union: default arm ok
static_assert(std::is_trivially_copyable_v<engine::platform::Event>);                // D8 invariant
static_assert(std::is_nothrow_default_constructible_v<engine::platform::InputState>);
static_assert(std::is_default_constructible_v<engine::platform::InputState>);

// ---- task 0.3.3 audio surface ----
static_assert(engine::platform::AudioDeviceConfig{}.sampleRate == 48000U);
static_assert(engine::platform::AudioDeviceConfig{}.channels == 2U);
static_assert(!engine::platform::AudioDeviceConfig{}.headless);
static_assert(!std::is_copy_constructible_v<engine::platform::AudioDevice>);  // move-only RAII
static_assert(std::is_move_constructible_v<engine::platform::AudioDevice>);

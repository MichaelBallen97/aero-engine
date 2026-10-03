#pragma once
// Aero Engine — the editor's embedded font data (task E.6.1). SRC-PRIVATE. Each accessor is DEFINED by a TU
// that cmake/embed_run.cmake generates from editor/third_party/fonts at build time (never committed, never
// linted); the bytes are upstream's, unmodified -- that directory's README pins every SHA-256. Static
// storage for the program's life, which is why ImGui is handed them as NOT owned (editor_fonts.cpp).
// Functions rather than `extern const` arrays: a non-constexpr namespace-scope constant falls under
// readability-identifier-naming's camelBack VariableCase in this tree's .clang-tidy, and a span carries its
// own size.
#include <cstdint>
#include <span>

namespace engine::editor::embedded {
[[nodiscard]] std::span<const std::uint8_t> ibmPlexSansRegular() noexcept;
[[nodiscard]] std::span<const std::uint8_t> ibmPlexSansSemiBold() noexcept;
[[nodiscard]] std::span<const std::uint8_t> ibmPlexMonoRegular() noexcept;
[[nodiscard]] std::span<const std::uint8_t> lucideIcons() noexcept;
}  // namespace engine::editor::embedded

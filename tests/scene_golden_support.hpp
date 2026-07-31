#pragma once
// tests/scene_golden_support.hpp -- task 2.5.2: the shared golden-fixture assertion helpers.
// TESTS-LOCAL; never an engine header, never shipped, never linked into anything but a test binary.
//
// SHARED rather than TU-local-copied (spec D6), against the repo's usual convention. That convention
// is right for SCAFFOLDING -- four copies of TempDir exist and should. This helper IS THE ASSERTION:
// two copies that drifted would mean the engine battery (tests/scene_serialize_test.cpp) and the
// editor battery (tests/editor/scene_golden_test.cpp) had silently stopped checking the same thing.
// The precedent for a shared tests-local header is tests/rhi_test_support.hpp (task 0.4.2).
//
// It deals ONLY in std::string / std::string_view / std::filesystem and names NO engine type at all.
// That is what lets the editor battery include it without putting the engine's serialization module
// on a tests/editor/ compile line -- a rule task 2.5.1 established and polices with a token grep.
//
// Four things CI enforces here, each learned the hard way at least once in this project:
//   * <cstdint> is included EXPLICITLY: libstdc++ does not supply it transitively.
//   * NOTHING recurses. misc-no-recursion is --warnings-as-errors on the Linux lane, and this header
//     sits inside .clang-tidy's HeaderFilterRegex, so its diagnostics surface through both .cpp files
//     that include it even though CI never lints a .hpp directly.
//   * File-scope constants are SCREAMING_SNAKE (ConstexprVariableCase: UPPER_CASE, which applies to
//     locals too); everything else is plain camelBack with NO trailing underscore.
//   * Every std::filesystem call uses the std::error_code overload. A diagnostic helper must never
//     throw, and must never itself fail a test.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace scene_golden {

namespace detail {

// The tree's standing UTF-8 -> path construction (engine/core/src/vfs.cpp, editor/src/
// project_files.cpp, editor/src/scene_file.cpp:33-36). NOT std::filesystem::u8path -- deprecated
// since C++20. Construct from UTF-8 BYTES so a non-ASCII directory resolves on Windows, where path's
// native encoding is UTF-16 and the narrow-char constructor assumes the ACTIVE CODE PAGE, not UTF-8.
[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr std::size_t CONTEXT_BYTES = 48;

// One byte, rendered ASCII-safe. EVERY byte outside printable ASCII becomes an escape -- a superset
// of the spec's "C0 bytes as \xNN", and necessary here: edge.scene.json carries 2-, 3- and 4-byte
// UTF-8, and a +/-48-byte context window routinely cuts a sequence in half. Emitting the halves raw
// would put invalid UTF-8 into the doctest output of every failure that lands near a name.
inline void appendByte(std::string& out, char raw) {
    const auto value = static_cast<unsigned char>(raw);
    if (value >= 0x20U && value < 0x7FU) {
        out += raw;
        return;
    }
    out += "\\x";
    out += HEX_DIGITS[(value >> 4U) & 0x0FU];
    out += HEX_DIGITS[value & 0x0FU];
}

inline void appendRange(std::string& out, std::string_view text, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        appendByte(out, text[i]);
    }
}

}  // namespace detail

// A binary read. ok == false leaves `error` set and `text` empty.
// NEVER text mode: the whole task is a byte comparison, and text mode on Windows rewrites every
// newline on the way in -- which is exactly the regression the editor battery exists to catch.
struct FileBytes {
    bool ok = false;
    std::string text;
    std::string error;
};

[[nodiscard]] inline FileBytes readBytes(std::string_view path) {
    std::ifstream in(detail::pathFromUtf8(path), std::ios::binary);
    if (!in) {
        return {false, {}, "cannot open " + std::string(path)};
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // .bad(), NOT .fail(): fail() is set by the EOF that ends the istreambuf_iterator loop above,
    // which is the ordinary, successful termination (editor/src/scene_file.cpp:61-65's rule).
    if (in.bad()) {
        return {false, {}, "read failed for " + std::string(path)};
    }
    return {true, std::move(text), {}};
}

// "" == clean. Otherwise names the ONE violated property, with a byte offset wherever one exists.
// AC-1's properties, checked in the order a human wants to hear about them. UTF-8 VALIDITY is
// deliberately NOT checked here -- it is proven by file(1) in the shell gate and, far more strongly,
// by G7's byte-exact comparison of both exotic names against C++ literals.
[[nodiscard]] inline std::string hygieneComplaint(std::string_view text) {
    if (text.empty()) {
        return "fixture is empty";
    }
    if (text.size() >= 3U && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU && static_cast<unsigned char>(text[2]) == 0xBFU) {
        return "UTF-8 BOM at offset 0";
    }
    const std::size_t carriage = text.find('\r');
    if (carriage != std::string_view::npos) {
        return "carriage return at offset " + std::to_string(carriage);
    }
    const std::size_t tab = text.find('\t');
    if (tab != std::string_view::npos) {
        return "raw tab at offset " + std::to_string(tab);
    }
    if (text.back() != '\n') {
        return "no trailing newline (last byte at offset " + std::to_string(text.size() - 1U) + ")";
    }
    if (text.size() >= 2U && text[text.size() - 2U] == '\n') {
        return "more than one trailing newline (offset " + std::to_string(text.size() - 2U) + ")";
    }
    return {};
}

// The first differing byte offset, with +/-48 bytes of context from BOTH sides. "" when equal.
// Fed to doctest's INFO, which evaluates its argument LAZILY inside a by-reference lambda
// (doctest.h:3140-3145 wraps it in MakeContextScope), so this costs nothing on a green run.
[[nodiscard]] inline std::string describeMismatch(std::string_view expected, std::string_view actual) {
    if (expected == actual) {
        return {};
    }
    const std::size_t shorter = (expected.size() < actual.size()) ? expected.size() : actual.size();
    std::size_t offset = 0;
    while (offset < shorter && expected[offset] == actual[offset]) {
        ++offset;
    }
    const std::size_t begin = (offset > detail::CONTEXT_BYTES) ? offset - detail::CONTEXT_BYTES : 0U;
    const std::size_t expectedEnd =
        (offset + detail::CONTEXT_BYTES < expected.size()) ? offset + detail::CONTEXT_BYTES : expected.size();
    const std::size_t actualEnd =
        (offset + detail::CONTEXT_BYTES < actual.size()) ? offset + detail::CONTEXT_BYTES : actual.size();

    std::string out = "golden mismatch at byte offset ";
    out += std::to_string(offset);
    out += " (expected ";
    out += std::to_string(expected.size());
    out += " bytes, actual ";
    out += std::to_string(actual.size());
    out += " bytes)\n  expected: ";
    detail::appendRange(out, expected, begin, expectedEnd);
    out += "\n  actual  : ";
    detail::appendRange(out, actual, begin, actualEnd);
    return out;
}

// Best-effort dump to <outDir>/<name>.actual.scene.json, to make a remote-lane failure debuggable
// locally. SILENT on every failure: a diagnostic must never itself fail a test. outDir differs per
// battery (spec D13) because `ctest -j` runs the two host targets concurrently.
inline void dumpActual(std::string_view outDir, std::string_view name, std::string_view actual) {
    std::error_code ec;
    const std::filesystem::path dir = detail::pathFromUtf8(outDir);
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }
    std::ofstream out(dir / (std::string(name) + ".actual.scene.json"), std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out.write(actual.data(), static_cast<std::streamsize>(actual.size()));
}

}  // namespace scene_golden

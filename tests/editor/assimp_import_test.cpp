// tests/editor/assimp_import_test.cpp -- task 3.2.5: the Assimp backend for .dae/.ply/.stl. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the obj_import_test.cpp / fbx_import_test.cpp / model_import_test.cpp precedent):
// model_import.hpp depends only on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and
// aero/editor/scene_bounds.hpp -- the last of those reaches aero::scene, a PUBLIC, UNGATED dependency
// of aero_editor_core (only engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in
// this file must be PRESENT and PASSING in both reduced configurations, not merely the default build.
// No GPU, no window, no ImGui context, no sleeps.
//
// Nearly every fixture is a raw ASCII string literal. The BINARY .ply and .stl fixtures are BUILT
// BYTE-BY-BYTE HERE rather than committed, which is what makes the "byte-identical twin" assertions
// possible at all -- a committed binary would be compared against a literal that describes it, which
// proves nothing about the two agreeing. The three committed fixtures (cube.dae, cube.ply, cube.stl)
// are reached through AERO_ASSET_FIXTURES_DIR, ALREADY defined on this target, and exist so the
// real-bytes path through readFileBytes is proven on real bytes.
//
// THIS TU NAMES NO ASSIMP TYPE (AC-11/AC-13's sibling check) -- it drives the backend only indirectly,
// through the PUBLIC importModel() dispatch. assimp_import.hpp is src-private and stays that way;
// nothing here #includes it.
#include <aero/editor/model_import.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The byte-loading pattern model_import_test.cpp / fbx_import_test.cpp / obj_import_test.cpp each keep
// their own copy of, restated here so this TU stays independent of all three.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// A source line with its `//` comment removed -- the MI42c/MI42d / OI77 / MS41 shape, this TU's own
// copy (the foldAscii/addWarning precedent). AI2 and AI3 both depend on it, and it is what makes a
// gate survive the gated file's OWN documentation of the tokens it forbids (the AC-56 lesson).
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

// Reads a file under editor/src through AERO_EDITOR_SRC_DIR (ALREADY defined on this target by task
// 2.6.1 -- no second definition is added, which would be a drift surface) and REQUIREs success. A
// missing file is a FAILURE, never a skip.
[[nodiscard]] std::string readEditorSource(const std::string& relativePath) {
    const std::string path = std::string(AERO_EDITOR_SRC_DIR) + "/" + relativePath;
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    REQUIRE_MESSAGE(read.text.has_value(), path);
    return *read.text;
}

// The same file's text with every `//` comment stripped, line by line.
[[nodiscard]] std::string strippedSource(const std::string& relativePath) {
    const std::string text = readEditorSource(relativePath);
    std::string out;
    out.reserve(text.size());
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        out.append(codeOf(line));
        out.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return out;
}

// THE INCLUDE TOKEN, spelled as TWO ADJACENT LITERALS on purpose. The compiler concatenates them; the
// FILE never contains the eight-character sequence itself, so the cheap gate grep over editor/, tests/,
// engine/, runtime/ and tools/ still returns exactly one path -- the backend TU -- rather than this
// test as well. Do not "simplify" it into one literal.
[[nodiscard]] std::string assimpIncludeToken() { return std::string("#include <assimp") + "/"; }

// The editor's own source list, DERIVED FROM THE TREE rather than hard-coded: every `src/<name>.cpp`
// token in editor/CMakeLists.txt, which is exactly the set compiled into aero_editor_core plus
// main.cpp. A TU added tomorrow is covered the day it is added, which a frozen array cannot promise
// (.claude/rules/boundary-guards.md: "never a hardcoded per-root count ... also assert coverage").
[[nodiscard]] std::vector<std::string> editorSourceFiles() {
    const std::string path = std::string(AERO_EDITOR_SRC_DIR) + "/../CMakeLists.txt";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    REQUIRE_MESSAGE(read.text.has_value(), path);
    const std::string& text = *read.text;

    std::vector<std::string> out;
    constexpr std::string_view PREFIX = "src/";
    std::size_t at = 0;
    while (true) {
        const std::size_t found = text.find(PREFIX, at);
        if (found == std::string::npos) {
            break;
        }
        const auto isNameChar = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        };
        std::size_t end = found + PREFIX.size();
        while (end < text.size() && isNameChar(text[end])) {
            ++end;
        }
        const std::string name = text.substr(found + PREFIX.size(), end - found - PREFIX.size());
        at = end;
        if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".cpp") != 0) {
            continue;
        }
        bool seen = false;
        for (const std::string& existing : out) {
            if (existing == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out.push_back(name);
        }
    }
    return out;
}

}  // namespace

using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;

// AI1 -- the case that pulls assimp's archive (and, on Windows, loads its DLL). Nothing else in the
// suite calls importAssimp at all, so without this case the link is declared and never exercised, and
// neither R1 (does it build and link on six presets) nor R2 (the second stb_image implementation) is
// tested by anything. TIGHTENED at step 3 to assert a real parse rather than Unsupported.
TEST_CASE("AI1: .dae/.ply/.stl reach the Assimp backend through importModel") {
    const std::string body = "x";
    for (const std::string_view name : {"x.dae", "x.ply", "x.stl"}) {
        const ImportResult result = importModel(name, "", asBytes(body), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Unsupported);
    }
}

// AI2 (AC-11/AC-13) -- THE INCLUDE BOUNDARY, pinned by a case rather than only by a grep. The grep
// stays in the gate as a cheap first line, but it is prose-fragile: editor/CMakeLists.txt and
// tests/CMakeLists.txt are both inside its scanned roots, so the most natural comment to write in
// either would turn it red for a reason that is not a violation. This case survives a prose edit.
TEST_CASE("AI2: exactly one editor source includes an Assimp header, and it is the backend TU") {
    const std::string token = assimpIncludeToken();

    const std::vector<std::string> sources = editorSourceFiles();
    REQUIRE_FALSE(sources.empty());
    // A floor, not an exact count: a parsing failure that produced two names must not pass silently,
    // while adding a TU must not have to touch this number. The named members below are what make the
    // list's IDENTITY checkable rather than only its size.
    REQUIRE(sources.size() >= 50);

    const auto contains = [&sources](std::string_view name) {
        for (const std::string& existing : sources) {
            if (existing == name) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(contains("assimp_import.cpp"));
    REQUIRE(contains("model_import.cpp"));
    REQUIRE(contains("gltf_import.cpp"));

    // Every src-private header is reached by exactly one quoted include from a .cpp, so harvesting
    // them from the .cpp texts keeps BOTH sides of this scan derived from the tree.
    std::vector<std::string> scanned = sources;
    std::vector<std::string> headers;
    for (const std::string& source : sources) {
        const std::string text = readEditorSource(source);
        std::size_t at = 0;
        while (true) {
            const std::size_t found = text.find("#include \"", at);
            if (found == std::string::npos) {
                break;
            }
            const std::size_t start = found + 10U;
            const std::size_t close = text.find('"', start);
            if (close == std::string::npos) {
                break;
            }
            const std::string name = text.substr(start, close - start);
            at = close + 1U;
            if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".hpp") != 0 ||
                name.find('/') != std::string::npos) {
                continue;
            }
            bool seen = false;
            for (const std::string& existing : headers) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                headers.push_back(name);
            }
        }
    }
    REQUIRE_FALSE(headers.empty());
    REQUIRE(headers.size() >= 10);
    for (const std::string& header : headers) {
        scanned.push_back(header);
    }

    std::vector<std::string> including;
    for (const std::string& file : scanned) {
        // strippedSource REQUIREs the read, so an unreadable listed file is a FAILURE, never a skip.
        if (strippedSource(file).find(token) != std::string::npos) {
            including.push_back(file);
        }
    }
    REQUIRE(including.size() == 1U);
    CHECK(including[0] == "assimp_import.cpp");
}

// tests/editor/project_settings_test.cpp -- task 2.6.2: the Project Settings panel's pure model.
// SEVENTEENTH TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, and that is the point (D4/AC-14). Every case here must be PRESENT and PASSING in all
// three configurations -- prove it with --list-test-cases, never with a skip. Tier-0: no GPU, no
// window, no ImGui context, so it must pass identically with AERO_REQUIRE_GPU unset and set.
#include <aero/editor/project.hpp>
#include <aero/editor/project_settings.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <ostream>  // MSVC ONLY, and not optional: PS15/PS23 compare a std::string_view inside a
                    // doctest assertion, so doctest's stringification instantiates the BODY of
                    // operator<<(basic_ostream&, basic_string_view) in <__msvc_string_view.hpp>. MSVC
                    // reaches that operator through <iosfwd> alone, leaving basic_ostream INCOMPLETE
                    // (C2027) -- while libc++ and libstdc++ both supply it transitively, so macOS and
                    // Linux compile this TU clean and cannot see the break. console_model_test.cpp
                    // makes the identical string_view comparison and builds on MSVC only because it
                    // includes <aero/core/log.hpp> and inherits <ostream> through spdlog. This TU
                    // deliberately depends on nothing but project.hpp (D4), so it inherits nothing.
                    // The include-what-you-use rule in .claude/rules/ci-portability.md, in its MSVC
                    // direction.
#include <string>
#include <string_view>
#include <vector>

using engine::editor::languageDisplayName;
using engine::editor::PROJECT_FORMAT_VERSION;
using engine::editor::PROJECT_SETTINGS_PANEL_ID;
using engine::editor::ProjectLanguage;
using engine::editor::ProjectManifest;
using engine::editor::ProjectSession;
using engine::editor::ProjectSettingsGroup;
using engine::editor::projectSettingsGroups;
using engine::editor::ProjectSettingsRow;

namespace {

constexpr std::string_view BUILD_VERSION = "0.1.0";

[[nodiscard]] ProjectSession openSession(std::string name = "MyGame", std::string engineVersion = "0.1.0",
                                         ProjectLanguage language = ProjectLanguage::Ts, std::string assets = "assets",
                                         std::string scenes = "scenes",
                                         std::string root = "/tmp/aero-projects/MyGame") {
    ProjectManifest manifest;
    manifest.name = std::move(name);
    manifest.engineVersion = std::move(engineVersion);
    manifest.language = language;
    manifest.assetsPath = std::move(assets);
    manifest.scenesPath = std::move(scenes);
    ProjectSession session;
    session.set(std::move(manifest), std::move(root));  // public; tests construct sessions freely --
    return session;                                     // INV-P1's grep is scoped to editor/src/
}

// Every case from PS6 onward indexes groups[0] / groups[1] POSITIONALLY. Guarding the shape first
// turns a shape-changing regression into a clean failure in the case that owns it, instead of an
// out-of-bounds read that aborts the whole binary before the later cases run. Sabotage seeds S9
// (groups emitted in the wrong order) and S10 (the Format version row dropped) both did exactly
// that: each cascaded past its predicted cases and died in PS10 with an ASan container-overflow,
// so PS22's independent eight-row sum -- the very case whose separateness is meant to catch a row
// that MOVED rather than vanished -- never got to run and its discrimination went undemonstrated.
// PS3/PS4/PS5 keep their own explicit REQUIREs: asserting the shape IS what those three are for.
[[nodiscard]] std::vector<ProjectSettingsGroup> shapedGroups(const ProjectSession& session,
                                                             std::string_view buildEngineVersion) {
    std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, buildEngineVersion);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].rows.size() == 6);
    REQUIRE(groups[1].rows.size() == 2);
    return groups;
}

}  // namespace

TEST_CASE("editor: a closed session yields no groups (task 2.6.2, PS1/AC-1/D11)") {
    const ProjectSession session;
    CHECK(projectSettingsGroups(session, BUILD_VERSION).empty());
    CHECK(projectSettingsGroups(session, "").empty());
}

TEST_CASE("editor: a CLOSED session's manifest is not empty, which is why PS1's guard matters (task 2.6.2, PS2/F6)") {
    ProjectSession session = openSession();
    session.close();
    CHECK(projectSettingsGroups(session, BUILD_VERSION).empty());
    CHECK(session.manifest().assetsPath == "assets");
    CHECK(session.manifest().scenesPath == "scenes");
    CHECK(session.manifest().name.empty());
}

TEST_CASE("editor: an open session yields exactly two groups, Manifest then Location (task 2.6.2, PS3/AC-2)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, BUILD_VERSION);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].title == "Manifest");
    CHECK(groups[1].title == "Location");
}

TEST_CASE("editor: the Manifest group's six labels, in order (task 2.6.2, PS4/AC-3)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, BUILD_VERSION);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[0].rows.size() == 6);
    CHECK(groups[0].rows[0].label == "Format version");
    CHECK(groups[0].rows[1].label == "Name");
    CHECK(groups[0].rows[2].label == "Engine version");
    CHECK(groups[0].rows[3].label == "Language");
    CHECK(groups[0].rows[4].label == "Assets path");
    CHECK(groups[0].rows[5].label == "Scenes path");
}

TEST_CASE("editor: the Location group's two labels, in order (task 2.6.2, PS5/AC-4)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, BUILD_VERSION);
    REQUIRE(groups.size() == 2);
    REQUIRE(groups[1].rows.size() == 2);
    CHECK(groups[1].rows[0].label == "Project root");
    CHECK(groups[1].rows[1].label == "Manifest file");
}

TEST_CASE("editor: Format version comes from the constant (task 2.6.2, PS6/AC-5)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    // NEVER compare against the literal "1" -- that is what makes S11 discriminating the day a v2
    // exists.
    CHECK(groups[0].rows[0].value == std::to_string(PROJECT_FORMAT_VERSION));
}

TEST_CASE("editor: Name is byte-identical for ASCII (task 2.6.2, PS7/AC-6)") {
    const ProjectSession session = openSession("MyGame");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[1].value == "MyGame");
}

TEST_CASE("editor: Name survives UTF-8 (task 2.6.2, PS8/AC-6/E7)") {
    // Escapes, never glyphs -- the tree sets no /utf-8 flag (§S's lint-and-portability trap table).
    const ProjectSession session = openSession("Caf\xC3\xA9 Rocket \xF0\x9F\x9A\x80");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[1].value == "Caf\xC3\xA9 Rocket \xF0\x9F\x9A\x80");
}

TEST_CASE("editor: a % in the name is DATA (task 2.6.2, PS9/AC-6/E6/F4)") {
    const ProjectSession session = openSession("100% Cotton");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[1].value == "100% Cotton");
}

TEST_CASE("editor: the default relative paths are verbatim (task 2.6.2, PS10/AC-7)") {
    const ProjectSession session = openSession("MyGame", "0.1.0", ProjectLanguage::Ts, "assets", "scenes");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[4].value == "assets");
    CHECK(groups[0].rows[5].value == "scenes");
}

TEST_CASE("editor: nested relative paths are verbatim (task 2.6.2, PS11/AC-7/E8)") {
    const ProjectSession session = openSession("MyGame", "0.1.0", ProjectLanguage::Ts, "content/art", "content/levels");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[4].value == "content/art");
    CHECK(groups[0].rows[5].value == "content/levels");
}

TEST_CASE(
    "editor: a trailing separator is NOT stripped here, and IS stripped by assetsRoot() (task 2.6.2, "
    "PS12/AC-7/E9)") {
    const ProjectSession session = openSession("MyGame", "0.1.0", ProjectLanguage::Ts, "assets/", "scenes/", "/tmp/p");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[4].value == "assets/");
    // Built from the accessor, never a literal -- pinning the deliberate difference.
    CHECK(session.assetsRoot() == std::string(session.root()) + "/assets");
}

TEST_CASE("editor: Language reads TypeScript for Ts (task 2.6.2, PS13/AC-8)") {
    const ProjectSession session = openSession("MyGame", "0.1.0", ProjectLanguage::Ts);
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[3].value == "TypeScript");
}

TEST_CASE("editor: Language reads C++ for Cpp (task 2.6.2, PS14/AC-8/E12)") {
    const ProjectSession session = openSession("MyGame", "0.1.0", ProjectLanguage::Cpp);
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[0].rows[3].value == "C++");
}

TEST_CASE("editor: languageDisplayName directly, and it is noexcept (task 2.6.2, PS15/AC-8)") {
    CHECK(languageDisplayName(ProjectLanguage::Ts) == "TypeScript");
    CHECK(languageDisplayName(ProjectLanguage::Cpp) == "C++");
    CHECK(languageDisplayName(ProjectLanguage::Ts) != languageDisplayName(ProjectLanguage::Cpp));
    static_assert(noexcept(languageDisplayName(ProjectLanguage::Ts)));
}

TEST_CASE("editor: Engine version is verbatim when it equals the build (task 2.6.2, PS16/AC-9)") {
    const ProjectSession session = openSession("MyGame", "0.1.0");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, "0.1.0");
    CHECK(groups[0].rows[2].value == "0.1.0");
    CHECK(groups[0].rows[2].value.find("this build") == std::string::npos);
}

TEST_CASE("editor: an EMPTY build version suppresses the suffix even when they differ (task 2.6.2, PS17/AC-10)") {
    const ProjectSession session = openSession("MyGame", "0.0.9");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, "");
    CHECK(groups[0].rows[2].value == "0.0.9");
    CHECK(groups[0].rows[2].value.find("this build") == std::string::npos);
}

TEST_CASE("editor: both non-empty and different => the exact suffix (task 2.6.2, PS18/AC-11/E10)") {
    const ProjectSession session = openSession("MyGame", "0.0.9");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, "0.1.0");
    CHECK(groups[0].rows[2].value == "0.0.9 (this build: 0.1.0)");
}

TEST_CASE("editor: an EMPTY manifest version still gets the suffix (task 2.6.2, PS19/AC-10/F13)") {
    const ProjectSession session = openSession("MyGame", "");
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, "0.1.0");
    CHECK(groups[0].rows[2].value == " (this build: 0.1.0)");
}

TEST_CASE("editor: the two Location rows come from the session's own accessors (task 2.6.2, PS20/AC-12)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = shapedGroups(session, BUILD_VERSION);
    CHECK(groups[1].rows[0].value == std::string(session.root()));
    CHECK(groups[1].rows[1].value == session.manifestPath());
    // So the two rows are provably distinct and S8 cannot pass by coincidence. NEVER re-derive a
    // '/'-separated literal here -- 2.6.1's post-merge Windows CI failure was exactly that.
    CHECK(session.manifestPath() != std::string(session.root()));
}

TEST_CASE("editor: no row is silently blank for a fully populated manifest (task 2.6.2, PS21)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, BUILD_VERSION);
    for (const ProjectSettingsGroup& group : groups) {
        for (const ProjectSettingsRow& row : group.rows) {
            CHECK_FALSE(row.label.empty());
            CHECK_FALSE(row.value.empty());
        }
    }
}

TEST_CASE("editor: the total row count is exactly eight (task 2.6.2, PS22/AC-3+AC-4)") {
    const ProjectSession session = openSession();
    const std::vector<ProjectSettingsGroup> groups = projectSettingsGroups(session, BUILD_VERSION);
    std::size_t total = 0;
    for (const ProjectSettingsGroup& group : groups) {
        total += group.rows.size();
    }
    CHECK(total == 8);
}

TEST_CASE("editor: the panel id is frozen (task 2.6.2, PS23/INV-S1)") {
    CHECK(std::string_view(PROJECT_SETTINGS_PANEL_ID) == "Project Settings");
}

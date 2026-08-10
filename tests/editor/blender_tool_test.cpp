// tests/editor/blender_tool_test.cpp -- task 3.2.4: the PURE half of the Blender CLI integration.
// Tier-0, UNGATED, no GPU, no window, no ImGui context, and NO DISK ON THE CRITICAL PATH: every
// fixture here is a string literal or an injected struct. That is what makes AC-4 -- all three host
// platforms' candidate lists asserted from ONE test process -- possible at all.
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. blender_tool.hpp depends on neither reflection nor entt,
// so every case here must be PRESENT and PASSING in all three build configurations (AC-43) --
// prove it with --list-test-cases, never by assuming.
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/model_import.hpp>  // BT33: isImportableModelName -- the pair that matters

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using engine::editor::BLENDER_ABSOLUTE_MIN;
using engine::editor::BLENDER_MIN_SUPPORTED;
using engine::editor::blenderCandidatePaths;
using engine::editor::BlenderEnv;
using engine::editor::BlenderSupport;
using engine::editor::blenderSupport;
using engine::editor::BlenderVersion;
using engine::editor::currentHostOs;
using engine::editor::HostOs;
using engine::editor::isBlendFileName;
using engine::editor::isImportableModelName;
using engine::editor::MAX_PATH_ENTRIES_SCANNED;
using engine::editor::parseBlenderVersion;

namespace {

[[nodiscard]] bool contains(const std::vector<std::string>& list, std::string_view wanted) {
    return std::find(list.begin(), list.end(), std::string(wanted)) != list.end();
}

[[nodiscard]] long countOf(const std::vector<std::string>& list, std::string_view wanted) {
    return std::count(list.begin(), list.end(), std::string(wanted));
}

// The VERBATIM capture of a real `blender --version` on Blender 5.2.0 LTS (plan §G-4). Kept as a
// literal rather than a fixture file precisely so it cannot drift from what was measured.
constexpr std::string_view REAL_5_2_0_CAPTURE =
    "Blender 5.2.0 LTS\n"
    "\tbuild date: 2026-07-14\n"
    "\tbuild time: 01:31:22\n"
    "\tbuild commit date: 2026-07-13\n"
    "\tbuild commit time: 15:20\n"
    "\tbuild hash: fbe6228777e7\n";

}  // namespace

// ---------------------------------------------------------------------------------------------
// Host + candidates (BT1-BT16)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: currentHostOs returns one of the three desktop hosts and is noexcept (BT1, AC-5)") {
    static_assert(noexcept(currentHostOs()), "the one per-host branch must never be able to throw");
    const HostOs os = currentHostOs();
    CHECK((os == HostOs::Windows || os == HostOs::MacOs || os == HostOs::Linux));
}

TEST_CASE("blender_tool: HostOs has an explicit uint8 underlying type and fixed values (BT2)") {
    // performance-enum-size is --warnings-as-errors on the Linux lane; the explicit type is also what
    // makes these values a stable part of the contract rather than an implementation detail.
    static_assert(std::is_same_v<std::underlying_type_t<HostOs>, std::uint8_t>);
    CHECK(static_cast<std::uint8_t>(HostOs::Windows) == 0);
    CHECK(static_cast<std::uint8_t>(HostOs::MacOs) == 1);
    CHECK(static_cast<std::uint8_t>(HostOs::Linux) == 2);
}

TEST_CASE("blender_tool: BlenderSupport has an explicit uint8 underlying type and fixed values (BT3)") {
    static_assert(std::is_same_v<std::underlying_type_t<BlenderSupport>, std::uint8_t>);
    CHECK(static_cast<std::uint8_t>(BlenderSupport::Supported) == 0);
    CHECK(static_cast<std::uint8_t>(BlenderSupport::Warned) == 1);
    CHECK(static_cast<std::uint8_t>(BlenderSupport::Refused) == 2);
}

TEST_CASE("blender_tool: BlenderVersion defaults to 0.0.0 and orders component-major-first (BT4)") {
    constexpr BlenderVersion DEFAULTED;
    CHECK(DEFAULTED.major == 0);
    CHECK(DEFAULTED.minor == 0);
    CHECK(DEFAULTED.patch == 0);
    CHECK(BlenderVersion{2, 79, 9} < BLENDER_ABSOLUTE_MIN);
    CHECK(BlenderVersion{2, 80, 0} == BLENDER_ABSOLUTE_MIN);
    CHECK(BlenderVersion{3, 2, 9} < BLENDER_MIN_SUPPORTED);
    CHECK(BlenderVersion{3, 3, 0} == BLENDER_MIN_SUPPORTED);
    CHECK(BlenderVersion{5, 2, 0} > BLENDER_MIN_SUPPORTED);
    // major dominates minor dominates patch -- a lexicographic compare on the three members, which is
    // exactly what the defaulted <=> gives and what the version bands below depend on.
    CHECK(BlenderVersion{4, 0, 0} > BlenderVersion{3, 99, 99});
}

TEST_CASE("blender_tool: the Windows well-known list is generated from one test process (BT5, AC-2/AC-4)") {
    BlenderEnv env;
    env.programFiles = "C:/PF";
    env.localAppData = "C:/LAD";
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Windows, env);

    // 13 release lines under %PROGRAMFILES%, the same 13 under %LOCALAPPDATA%, then Steam.
    REQUIRE(candidates.size() == 27);
    CHECK(candidates[0] == "C:/PF\\Blender Foundation\\Blender 5.2\\blender.exe");
    CHECK(candidates[1] == "C:/PF\\Blender Foundation\\Blender 5.1\\blender.exe");
    CHECK(candidates[12] == "C:/PF\\Blender Foundation\\Blender 3.3\\blender.exe");
    CHECK(candidates[13] == "C:/LAD\\Programs\\Blender Foundation\\Blender 5.2\\blender.exe");
    CHECK(candidates[25] == "C:/LAD\\Programs\\Blender Foundation\\Blender 3.3\\blender.exe");
    CHECK(candidates[26] == "C:/PF\\Steam\\steamapps\\common\\Blender\\blender.exe");
    // DESCENDING within each family: a machine with several installs probes the newest first.
    CHECK(candidates[0].find("5.2") != std::string::npos);
    CHECK(candidates[12].find("3.3") != std::string::npos);
}

TEST_CASE("blender_tool: the macOS well-known list is generated from one test process (BT6, AC-2/AC-4)") {
    BlenderEnv env;
    env.homeDir = "/Users/dev";
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::MacOs, env);

    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0] == "/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(candidates[1] == "/Users/dev/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(candidates[2] == "/Applications/Blender/Blender.app/Contents/MacOS/Blender");
    // The macOS binary inside the bundle has NO extension -- which is also why the Locate... dialog
    // this task adds later carries no filter array at all.
    CHECK(candidates[0].find(".exe") == std::string::npos);
}

TEST_CASE("blender_tool: the Linux well-known list is generated from one test process (BT7, AC-2/AC-4)") {
    BlenderEnv env;
    env.homeDir = "/home/dev";
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);

    REQUIRE(candidates.size() == 6);
    CHECK(candidates[0] == "/usr/bin/blender");
    CHECK(candidates[1] == "/usr/local/bin/blender");
    CHECK(candidates[2] == "/snap/bin/blender");
    CHECK(candidates[3] == "/var/lib/flatpak/exports/bin/org.blender.Blender");
    CHECK(candidates[4] == "/home/dev/.local/share/flatpak/exports/bin/org.blender.Blender");
    CHECK(candidates[5] == "/opt/blender/blender");
}

TEST_CASE("blender_tool: a non-empty override yields EXACTLY ONE entry on every host (BT8, AC-3)") {
    for (const HostOs os : {HostOs::Windows, HostOs::MacOs, HostOs::Linux}) {
        BlenderEnv env;
        env.overridePath = "/configured/blender";
        env.pathEntries = {"/usr/bin", "/usr/local/bin"};
        env.homeDir = "/home/dev";
        env.programFiles = "C:/PF";
        env.localAppData = "C:/LAD";
        const std::vector<std::string> candidates = blenderCandidatePaths(os, env);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0] == "/configured/blender");
    }
}

TEST_CASE("blender_tool: a non-empty AERO_BLENDER_PATH yields EXACTLY ONE entry (BT9, AC-3)") {
    for (const HostOs os : {HostOs::Windows, HostOs::MacOs, HostOs::Linux}) {
        BlenderEnv env;
        env.envPath = "/from/env/blender";
        env.pathEntries = {"/usr/bin"};
        env.homeDir = "/home/dev";
        env.programFiles = "C:/PF";
        const std::vector<std::string> candidates = blenderCandidatePaths(os, env);
        REQUIRE(candidates.size() == 1);
        CHECK(candidates[0] == "/from/env/blender");
    }
}

TEST_CASE("blender_tool: the override BEATS the environment variable (BT10, AC-2)") {
    BlenderEnv env;
    env.overridePath = "/configured/blender";
    env.envPath = "/from/env/blender";
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == "/configured/blender");
    CHECK_FALSE(contains(candidates, "/from/env/blender"));
}

TEST_CASE("blender_tool: an override naming a path that does not exist STILL yields one entry (BT11, AC-3)") {
    // Purity, restated: this function never touches disk, so "does not exist" is not a concept it can
    // have. That is exactly the point -- a wrong configured path must produce "the Blender you
    // configured is not there", never a different Blender's output.
    BlenderEnv env;
    env.overridePath = "/definitely/not/here/blender";
    env.pathEntries = {"/usr/bin"};
    env.homeDir = "/home/dev";
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == "/definitely/not/here/blender");
    CHECK_FALSE(contains(candidates, "/usr/bin/blender"));
}

TEST_CASE("blender_tool: PATH entries take the per-host executable name, in PATH order, first (BT12, AC-2)") {
    // The SPLITTING of PATH on ';' vs ':' belongs to readBlenderEnv (task 3.2.4 step 5, BS-series):
    // this function receives entries ALREADY split, by contract. What is decidable HERE -- and what
    // this case pins -- is the per-host executable name and the join, from one test process.
    BlenderEnv env;
    env.pathEntries = {"/a", "/b"};
    env.programFiles = "C:/PF";
    env.localAppData = "C:/LAD";
    env.homeDir = "/home/dev";

    const std::vector<std::string> windows = blenderCandidatePaths(HostOs::Windows, env);
    REQUIRE(windows.size() >= 2);
    CHECK(windows[0] == "/a/blender.exe");
    CHECK(windows[1] == "/b/blender.exe");

    const std::vector<std::string> mac = blenderCandidatePaths(HostOs::MacOs, env);
    REQUIRE(mac.size() >= 2);
    CHECK(mac[0] == "/a/blender");
    CHECK(mac[1] == "/b/blender");

    const std::vector<std::string> linux = blenderCandidatePaths(HostOs::Linux, env);
    REQUIRE(linux.size() >= 2);
    CHECK(linux[0] == "/a/blender");
    CHECK(linux[1] == "/b/blender");
}

TEST_CASE("blender_tool: empty and whitespace-only PATH entries are SKIPPED, never joined (BT13, E2)") {
    BlenderEnv env;
    env.pathEntries = {"", "   ", "\t", "/opt/bin", "\r\n"};
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);

    CHECK(contains(candidates, "/opt/bin/blender"));
    // The failure this guards is a PATH entry joined into a bare "/blender" -- a real absolute path
    // that names the root of the filesystem, which on a Linux CI box is a plausible stat target.
    CHECK_FALSE(contains(candidates, "/blender"));
    CHECK_FALSE(contains(candidates, "   /blender"));
    CHECK_FALSE(contains(candidates, "\t/blender"));
    CHECK_FALSE(contains(candidates, "\r\n/blender"));
}

TEST_CASE("blender_tool: duplicates are removed and FIRST-SEEN order is preserved (BT14, AC-2)") {
    BlenderEnv env;
    // "/usr/bin" appears three times in PATH AND is also a well-known Linux location, so the same
    // string is produced by two different sources -- the dedup must keep the FIRST occurrence.
    env.pathEntries = {"/first", "/usr/bin", "/second", "/usr/bin", "/first"};
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);

    REQUIRE(candidates.size() >= 3);
    CHECK(candidates[0] == "/first/blender");
    CHECK(candidates[1] == "/usr/bin/blender");
    CHECK(candidates[2] == "/second/blender");
    CHECK(countOf(candidates, "/first/blender") == 1);
    CHECK(countOf(candidates, "/usr/bin/blender") == 1);  // PATH won it; the well-known table did not re-add it
    // Every entry is unique -- a stronger statement than "these three are".
    std::vector<std::string> sorted = candidates;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}

TEST_CASE("blender_tool: the PATH sweep is capped at MAX_PATH_ENTRIES_SCANNED (BT15, R3)") {
    BlenderEnv env;
    env.homeDir = "/home/dev";  // so the well-known tail is the FULL six, not the home-less five
    env.pathEntries.reserve(MAX_PATH_ENTRIES_SCANNED + 44);
    for (std::size_t i = 0; i < MAX_PATH_ENTRIES_SCANNED + 44; ++i) {
        env.pathEntries.push_back("/p" + std::to_string(i));
    }
    const std::vector<std::string> candidates = blenderCandidatePaths(HostOs::Linux, env);

    CHECK(contains(candidates, "/p0/blender"));
    CHECK(contains(candidates, "/p" + std::to_string(MAX_PATH_ENTRIES_SCANNED - 1) + "/blender"));
    CHECK_FALSE(contains(candidates, "/p" + std::to_string(MAX_PATH_ENTRIES_SCANNED) + "/blender"));
    CHECK_FALSE(contains(candidates, "/p" + std::to_string(MAX_PATH_ENTRIES_SCANNED + 43) + "/blender"));
    // 256 from PATH plus the six well-known Linux entries, one of which ("/usr/bin/blender") is not
    // produced by any "/pN" entry, so nothing collapses: 262.
    CHECK(candidates.size() == MAX_PATH_ENTRIES_SCANNED + 6);
}

TEST_CASE("blender_tool: an empty homeDir SKIPS every home-relative entry entirely (BT16, E2)") {
    const BlenderEnv macEnv;  // homeDir deliberately left empty
    const std::vector<std::string> mac = blenderCandidatePaths(HostOs::MacOs, macEnv);
    REQUIRE(mac.size() == 2);
    CHECK(mac[0] == "/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(mac[1] == "/Applications/Blender/Blender.app/Contents/MacOS/Blender");
    // The failure this guards: emitting the suffix with a MISSING prefix, which names a real
    // filesystem path that has nothing to do with Blender.
    CHECK_FALSE(contains(mac, "/Applications/Blender.app/Contents/MacOS/Blender/Applications"));

    const BlenderEnv linuxEnv;
    const std::vector<std::string> linux = blenderCandidatePaths(HostOs::Linux, linuxEnv);
    REQUIRE(linux.size() == 5);
    CHECK_FALSE(contains(linux, "/.local/share/flatpak/exports/bin/org.blender.Blender"));
    CHECK(linux[4] == "/opt/blender/blender");

    const BlenderEnv winEnv;  // no programFiles, no localAppData either -- the same rule, one family over
    const std::vector<std::string> windows = blenderCandidatePaths(HostOs::Windows, winEnv);
    CHECK(windows.empty());
}

// ---------------------------------------------------------------------------------------------
// Version + support (BT17-BT28)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: the real six-line 5.2.0 LTS --version capture parses to 5.2.0 (BT17, AC-6)") {
    const std::optional<BlenderVersion> parsed = parseBlenderVersion(REAL_5_2_0_CAPTURE);
    REQUIRE(parsed.has_value());
    CHECK(parsed->major == 5);
    CHECK(parsed->minor == 2);
    CHECK(parsed->patch == 0);
    // The tab-indented continuation lines are never reached: "2026" would parse as a major version.
    CHECK(parsed->major != 2026);
}

TEST_CASE("blender_tool: a bare three-component version parses (BT18, AC-6)") {
    const std::optional<BlenderVersion> parsed = parseBlenderVersion("Blender 4.2.1");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == BlenderVersion{4, 2, 1});
}

TEST_CASE("blender_tool: a multi-line 3.6.0 LTS shape parses to 3.6.0 (BT19, AC-6)") {
    const std::optional<BlenderVersion> parsed = parseBlenderVersion("Blender 3.6.0 LTS\n\tbuild date: 2023-06-30\n");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == BlenderVersion{3, 6, 0});
}

TEST_CASE("blender_tool: a two-component version fills patch with 0 (BT20, AC-6)") {
    const std::optional<BlenderVersion> parsed = parseBlenderVersion("Blender 4.2");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == BlenderVersion{4, 2, 0});

    const std::optional<BlenderVersion> single = parseBlenderVersion("Blender 4");
    REQUIRE(single.has_value());
    CHECK(*single == BlenderVersion{4, 0, 0});
}

TEST_CASE("blender_tool: empty text and a wrong-case prefix are nullopt (BT21, AC-6)") {
    CHECK_FALSE(parseBlenderVersion("").has_value());
    CHECK_FALSE(parseBlenderVersion("\n").has_value());
    CHECK_FALSE(parseBlenderVersion("blender 5.2.0").has_value());
    CHECK_FALSE(parseBlenderVersion("BLENDER 5.2.0").has_value());
}

TEST_CASE("blender_tool: a non-numeric tail and a missing prefix are nullopt (BT22, AC-6)") {
    CHECK_FALSE(parseBlenderVersion("Blender x").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender ").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender").has_value());
    CHECK_FALSE(parseBlenderVersion("5.2.0").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender v5.2.0").has_value());
    // A dot with no digit after it is a malformed component, not a two-component version.
    CHECK_FALSE(parseBlenderVersion("Blender 5.").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender 5.2.").has_value());
}

TEST_CASE("blender_tool: a leading-whitespace form is nullopt (BT23, AC-6)") {
    // The prefix is required AT POSITION 0. A line that does not begin with Blender's own banner is
    // not that banner -- accepting leading whitespace would accept the tab-indented "build hash:"
    // continuation lines of some other tool's output.
    CHECK_FALSE(parseBlenderVersion(" Blender 5.2.0").has_value());
    CHECK_FALSE(parseBlenderVersion("\tBlender 5.2.0").has_value());
    CHECK_FALSE(parseBlenderVersion("\nBlender 5.2.0").has_value());
}

TEST_CASE("blender_tool: a huge component neither overflows nor throws (BT24, AC-6)") {
    // 4294967295 is exactly UINT32_MAX and must still parse; one more digit does not fit and yields
    // nullopt rather than a saturated lie. Parsed digit-by-digit with a 64-bit accumulator, never
    // std::stoul (which throws).
    const std::optional<BlenderVersion> atMax = parseBlenderVersion("Blender 4294967295.0.0");
    REQUIRE(atMax.has_value());
    CHECK(atMax->major == 4294967295U);

    CHECK_FALSE(parseBlenderVersion("Blender 4294967296.0.0").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender 99999999999999999999999999.2.0").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender 5.99999999999999999999999999.0").has_value());
    CHECK_FALSE(parseBlenderVersion("Blender 5.2.99999999999999999999999999").has_value());
    // nullopt reaches blenderSupport, which ATTEMPTS rather than refuses (D14) -- so a version this
    // parser cannot express never bricks an install that works.
    CHECK(blenderSupport(parseBlenderVersion("Blender 4294967296.0.0")) == BlenderSupport::Supported);
}

TEST_CASE("blender_tool: below 2.80 is Refused (BT25, AC-7/D14)") {
    CHECK(blenderSupport(BlenderVersion{2, 79, 9}) == BlenderSupport::Refused);
    CHECK(blenderSupport(BlenderVersion{2, 79, 0}) == BlenderSupport::Refused);
    CHECK(blenderSupport(BlenderVersion{1, 0, 0}) == BlenderSupport::Refused);
    CHECK(blenderSupport(BlenderVersion{0, 0, 0}) == BlenderSupport::Refused);
}

TEST_CASE("blender_tool: [2.80, 3.3) is Warned (BT26, AC-7/D14)") {
    CHECK(blenderSupport(BlenderVersion{2, 80, 0}) == BlenderSupport::Warned);
    CHECK(blenderSupport(BlenderVersion{2, 93, 18}) == BlenderSupport::Warned);
    CHECK(blenderSupport(BlenderVersion{3, 2, 9}) == BlenderSupport::Warned);
}

TEST_CASE("blender_tool: 3.3 and above is Supported (BT27, AC-7/D14)") {
    CHECK(blenderSupport(BlenderVersion{3, 3, 0}) == BlenderSupport::Supported);
    CHECK(blenderSupport(BlenderVersion{4, 2, 1}) == BlenderSupport::Supported);
    CHECK(blenderSupport(BlenderVersion{5, 2, 0}) == BlenderSupport::Supported);
}

TEST_CASE("blender_tool: an unparseable version is Supported, never Refused (BT28, AC-7/D14/E4)") {
    // The single most consequential band. An unparseable version is far more likely to be a locale, a
    // build suffix or a fork than a Blender from 2011 -- refusing on it would break installs that
    // work, so the design ATTEMPTS. Seed S8 inverts this and must redden here.
    CHECK(blenderSupport(std::nullopt) == BlenderSupport::Supported);
    CHECK(blenderSupport(parseBlenderVersion("Blender x")) == BlenderSupport::Supported);
    CHECK(blenderSupport(parseBlenderVersion("")) == BlenderSupport::Supported);
}

// ---------------------------------------------------------------------------------------------
// isBlendFileName (BT29-BT33)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: isBlendFileName is ASCII case-folded (BT29, AC-28)") {
    CHECK(isBlendFileName("a.blend"));
    CHECK(isBlendFileName("a.BLEND"));
    CHECK(isBlendFileName("a.Blend"));
    CHECK(isBlendFileName("A.bLeNd"));
}

TEST_CASE("blender_tool: isBlendFileName is a SUFFIX test on the full name (BT30, AC-28)") {
    CHECK(isBlendFileName("a.tar.blend"));
    CHECK(isBlendFileName("props/chair.blend"));
    CHECK_FALSE(isBlendFileName("a.blend.bak"));
    CHECK_FALSE(isBlendFileName("blender"));
    CHECK_FALSE(isBlendFileName("a.blender"));
    CHECK_FALSE(isBlendFileName("blend"));
}

TEST_CASE("blender_tool: a bare '.blend' is not a blend file (BT31, AC-28)") {
    // The isMetaFileName shape: something must PRECEDE the extension, or a dotfile named exactly
    // ".blend" would claim to be a model.
    CHECK_FALSE(isBlendFileName(".blend"));
    CHECK_FALSE(isBlendFileName(""));
}

TEST_CASE("blender_tool: Blender's own numbered backups are NOT source assets (BT32, AC-28/E25)") {
    CHECK_FALSE(isBlendFileName("a.blend1"));
    CHECK_FALSE(isBlendFileName("a.blend2"));
    CHECK_FALSE(isBlendFileName("a.BLEND1"));
}

TEST_CASE("blender_tool: a .blend is a blend file AND is NOT an importable model (BT33, AC-29/D15)") {
    // The two predicates are DELIBERATELY separate, and that separation is the whole of D15: phase
    // 7.5 gates its probe on isImportableModelName, so a .blend is skipped by code that already
    // exists and asset_database.cpp is not modified by this task at all. MI133 pins the second half
    // together with modelImporterNeedsExternalBuffers; this case pins the pair from the Blender side.
    CHECK(isBlendFileName("statue.blend"));
    CHECK_FALSE(isImportableModelName("statue.blend"));
    CHECK(isImportableModelName("statue.gltf"));
    CHECK_FALSE(isBlendFileName("statue.gltf"));
}

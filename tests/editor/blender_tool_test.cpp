// tests/editor/blender_tool_test.cpp -- task 3.2.4: the PURE half of the Blender CLI integration.
// Tier-0, UNGATED, no GPU, no window, no ImGui context, and NO DISK ON THE CRITICAL PATH: every
// fixture here is a string literal or an injected struct. That is what makes AC-4 -- all three host
// platforms' candidate lists asserted from ONE test process -- possible at all.
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. blender_tool.hpp depends on neither reflection nor entt,
// so every case here must be PRESENT and PASSING in all three build configurations (AC-43) --
// prove it with --list-test-cases, never by assuming.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME -- BT63's own "Library" (code review)
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/model_import.hpp>  // BT33: isImportableModelName -- the pair that matters

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using engine::ContentHash;
using engine::formatContentHash;
using engine::formatGuid;
using engine::Guid;
using engine::parseContentHash;
using engine::parseGuid;
using engine::editor::BLENDER_ABSOLUTE_MIN;
using engine::editor::BLENDER_EXPORT_DIR_NAME;
using engine::editor::BLENDER_MIN_SUPPORTED;
using engine::editor::BLENDER_SCRIPT_VERSION;
using engine::editor::blenderCandidatePaths;
using engine::editor::BlenderEnv;
using engine::editor::blenderExportDir;
using engine::editor::blenderExportScriptText;
using engine::editor::BlenderSupport;
using engine::editor::blenderSupport;
using engine::editor::BlenderVersion;
using engine::editor::buildExportArgs;
using engine::editor::buildVersionArgs;
using engine::editor::currentHostOs;
using engine::editor::EXPORT_PROVENANCE_FORMAT_VERSION;
using engine::editor::ExportProvenance;
using engine::editor::ExportStatus;
using engine::editor::HostOs;
using engine::editor::isBlendFileName;
using engine::editor::isImportableModelName;
using engine::editor::MAX_PATH_ENTRIES_SCANNED;
using engine::editor::parseBlenderVersion;
using engine::editor::parseExportProvenance;
using engine::editor::parseExportStatus;
using engine::editor::parseToolPrefs;
using engine::editor::provenanceMatches;
using engine::editor::TOOL_PREFS_FORMAT_VERSION;
using engine::editor::ToolPrefs;
using engine::editor::writeExportProvenanceText;
using engine::editor::writeToolPrefsText;

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

// The seven committed TEXT fixtures (AC-42: no binary file is added to this repository, ever).
constexpr std::string_view STATUS_OK_FIXTURE = AERO_ASSET_FIXTURES_DIR "/blender-status-ok.json";
constexpr std::string_view STATUS_FAILED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/blender-status-failed.json";
constexpr std::string_view STATUS_DAMAGED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/blender-status-damaged.json";
constexpr std::string_view PROVENANCE_FIXTURE = AERO_ASSET_FIXTURES_DIR "/blender-provenance.json";
constexpr std::string_view PROVENANCE_WRONG_VERSION_FIXTURE =
    AERO_ASSET_FIXTURES_DIR "/blender-provenance-wrong-version.json";
constexpr std::string_view TOOL_PREFS_FIXTURE = AERO_ASSET_FIXTURES_DIR "/editor-tools.json";
constexpr std::string_view TOOL_PREFS_DAMAGED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/editor-tools-damaged.json";

// blender-provenance.json's own pinned values, spelled ONCE here so no case can disagree with another.
constexpr std::string_view FIXTURE_GUID_TEXT = "0123456789abcdef0123456789abcdef";
constexpr std::string_view FIXTURE_SOURCE_HASH_TEXT = "fedcba9876543210fedcba9876543210";
constexpr std::string_view FIXTURE_FINGERPRINT_TEXT = "00112233445566778899aabbccddeeff";

// A fully-populated record, so every case below starts from something that MATCHES and flips exactly
// one axis. A case that starts from two differences cannot say which one it caught.
[[nodiscard]] ExportProvenance baselineProvenance() {
    ExportProvenance record;
    record.guid = *parseGuid(FIXTURE_GUID_TEXT);
    record.sourceHash = *parseContentHash(FIXTURE_SOURCE_HASH_TEXT);
    record.sourcePath = "props/chair.blend";
    record.blenderPath = "/Applications/Blender.app/Contents/MacOS/Blender";
    record.blenderVersion = "5.2.0 LTS";
    record.scriptVersion = 1;
    record.settingsFingerprint = std::string(FIXTURE_FINGERPRINT_TEXT);
    record.artifactBytes = 1936;
    return record;
}

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

// ---------------------------------------------------------------------------------------------
// argv + script (BT34-BT43)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: buildVersionArgs is exactly {binary, --version} (BT34, AC-10)") {
    const std::vector<std::string> args = buildVersionArgs("/opt/blender/blender");
    REQUIRE(args.size() == 2);
    CHECK(args[0] == "/opt/blender/blender");
    CHECK(args[1] == "--version");
}

TEST_CASE("blender_tool: buildExportArgs produces fifteen entries in a fixed order (BT35, AC-11)") {
    const std::vector<std::string> args =
        buildExportArgs("/bin/blender", "/proj/assets/chair.blend", "/proj/Library/BlenderExports/export_gltf.py",
                        "/proj/Library/BlenderExports/abc.glb", "/proj/Library/BlenderExports/abc.json");

    // Element by element, in order, one at a time -- so a failure NAMES THE POSITION rather than
    // dumping two vectors at each other. An order swap (seed S3) is invisible to a set comparison.
    REQUIRE(args.size() == 15);
    CHECK(args[0] == "/bin/blender");
    CHECK(args[1] == "-b");
    CHECK(args[2] == "/proj/assets/chair.blend");
    CHECK(args[3] == "-X");
    CHECK(args[4] == "-Y");
    CHECK(args[5] == "-noaudio");
    CHECK(args[6] == "--python-exit-code");
    CHECK(args[7] == "42");
    CHECK(args[8] == "--python");
    CHECK(args[9] == "/proj/Library/BlenderExports/export_gltf.py");
    CHECK(args[10] == "--");
    CHECK(args[11] == "--out");
    CHECK(args[12] == "/proj/Library/BlenderExports/abc.glb");
    CHECK(args[13] == "--status");
    CHECK(args[14] == "/proj/Library/BlenderExports/abc.json");
    // The .blend comes BEFORE the script, and every script parameter comes AFTER "--": Blender parses
    // its own flags up to that separator and hands the rest to sys.argv.
    CHECK(args[2].find(".blend") != std::string::npos);
    CHECK(std::find(args.begin(), args.end(), std::string("--")) - args.begin() == 10);
}

TEST_CASE("blender_tool: a hostile path survives byte-identically as ONE argv entry (BT36, AC-12)") {
    // A space, a single quote, a double quote, a backslash and non-ASCII bytes. Hoisted into a NAMED
    // LOCAL rather than written inline in the macro argument: a raw string literal containing \" breaks
    // MSVC's legacy preprocessor inside a doctest macro (the ci-portability rule).
    // The literals are split at every hex escape on purpose: "\xBC" followed by 'b' would swallow the
    // 'b' as a third hex digit and become an out-of-range escape -- a COMPILE error, and one this case
    // hit for real while being written. U+00FC is the two bytes C3 BC in UTF-8.
    const std::string hostile =
        "/tmp/dir with space/\xC3\xBC"
        "nicode/my c\xC3\xBC"
        "be's \"copy\"\\weird.blend";
    const std::vector<std::string> args = buildExportArgs("/bin/blender", hostile, "/s.py", "/o.glb", "/st.json");

    REQUIRE(args.size() == 15);
    CHECK(args[2] == hostile);  // BYTE-IDENTICAL -- nothing quoted, escaped or substituted here
    CHECK(args[2].size() == hostile.size());
    // Exactly ONE entry carries it: a shell-style split on the space would produce two, which is the
    // failure this case exists to make impossible. SDL owns Windows quoting; we must not pre-quote.
    CHECK(std::count(args.begin(), args.end(), hostile) == 1);
    CHECK(args[2].find('\'') != std::string::npos);
    CHECK(args[2].find('"') != std::string::npos);
    CHECK(args[2].find('\\') != std::string::npos);
    CHECK(args[2].find(' ') != std::string::npos);
}

TEST_CASE("blender_tool: an empty binary still produces a well-formed argv vector (BT37, AC-11)") {
    const std::vector<std::string> args = buildExportArgs("", "/b.blend", "/s.py", "/o.glb", "/st.json");
    REQUIRE(args.size() == 15);
    CHECK(args[0].empty());  // the caller's problem to reject, never a collapsed vector here
    CHECK(args[1] == "-b");
    CHECK(args[14] == "/st.json");
}

TEST_CASE("blender_tool: the argv builders are pure -- two calls are identical (BT38, AC-1)") {
    CHECK(buildVersionArgs("/bin/blender") == buildVersionArgs("/bin/blender"));
    CHECK(buildExportArgs("/a", "/b", "/c", "/d", "/e") == buildExportArgs("/a", "/b", "/c", "/d", "/e"));
    CHECK(buildExportArgs("/a", "/b", "/c", "/d", "/e") != buildExportArgs("/a", "/b", "/c", "/d", "/X"));
}

TEST_CASE("blender_tool: the export script has NO interpolation site of any kind (BT39, AC-13)") {
    const std::string script(blenderExportScriptText());
    // The plan's shorthand for this case says "no %, no {}, no format". The first two are literally
    // true and are asserted as such; the third is shorthand for "no SUBSTITUTION POINT", because the
    // script legitimately contains `traceback.format_exc()` and the key `"export_format"` -- neither
    // is a place anything can be interpolated. The checkable statements are these four.
    CHECK(script.find('%') == std::string::npos);         // no printf-style site
    CHECK(script.find("{}") == std::string::npos);        // no std::format-style placeholder
    CHECK(script.find(".format(") == std::string::npos);  // no Python str.format call
    CHECK(script.find("f\"") == std::string::npos);       // no f-string prefix
    CHECK(script.find("f'") == std::string::npos);
    // And the whole point of the above: every parameter arrives through sys.argv, never through the
    // text (D8 -- a `--python-expr` built by concatenation would be a code injection).
    CHECK(script.find("sys.argv") != std::string::npos);
}

TEST_CASE("blender_tool: the export script carries the RNA filter, GLB and its try/except (BT40, AC-14)") {
    const std::string script(blenderExportScriptText());
    // The RNA filter is D10's WHOLE mechanism: it is what lets one script span the supported version
    // range without a version table anywhere. Seed S18 removes it.
    CHECK(script.find("get_rna_type().properties") != std::string::npos);
    CHECK(script.find("\"export_format\": \"GLB\"") != std::string::npos);
    CHECK(script.find("try:") != std::string::npos);
    CHECK(script.find("except Exception:") != std::string::npos);
    CHECK(script.find("traceback.format_exc()") != std::string::npos);
    // The status file is written on BOTH paths, which is what makes "exit 0, ok: false" reachable.
    CHECK(script.find("json.dump(report, f)") != std::string::npos);
}

TEST_CASE("blender_tool: the export script does NOT mention export_yup (BT41, F6)") {
    // Blender's world is Z-up, glTF is Y-up, and the exporter's own default-enabled export_yup does
    // that conversion INSIDE Blender -- so what lands in the GLB is conformant glTF and 3.2.1's "the
    // importer converts NOTHING" rule holds. Setting it False (seed S19) would silently rotate every
    // imported model, with no test outside this one able to see it.
    const std::string script(blenderExportScriptText());
    CHECK(script.find("export_yup") == std::string::npos);
    CHECK(script.find("yup") == std::string::npos);
}

TEST_CASE("blender_tool: --factory-startup is a command-line flag, never a script line (BT42, AC-14)") {
    const std::string script(blenderExportScriptText());
    CHECK(script.find("--factory-startup") == std::string::npos);
    CHECK(script.find("factory") == std::string::npos);
    // It is on the argv instead, spelled -X. The bundled glTF exporter is factory-ENABLED, so no
    // --addons flag is needed either (VERIFIED against 5.2.0 LTS).
    const std::vector<std::string> args = buildExportArgs("/bin/blender", "/b.blend", "/s.py", "/o.glb", "/st.json");
    CHECK(std::find(args.begin(), args.end(), std::string("-X")) != args.end());
    CHECK(std::find(args.begin(), args.end(), std::string("--addons")) == args.end());
}

TEST_CASE("blender_tool: the script text is byte-identical across calls and the version is 1 (BT43)") {
    const std::string_view first = blenderExportScriptText();
    const std::string_view second = blenderExportScriptText();
    CHECK(first == second);
    CHECK(first.data() == second.data());  // the SAME compile-time constant, not a rebuilt string
    CHECK_FALSE(first.empty());
    CHECK(BLENDER_SCRIPT_VERSION == 1);
}

// ---------------------------------------------------------------------------------------------
// Tool preferences (BT44-BT49)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: tool preferences round-trip through the committed fixture (BT44, AC-8)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TOOL_PREFS_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<ToolPrefs> parsed = parseToolPrefs(fixture.text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->blenderPath == "/Applications/Blender.app/Contents/MacOS/Blender");

    const std::optional<ToolPrefs> reparsed = parseToolPrefs(writeToolPrefsText(*parsed));
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->blenderPath == parsed->blenderPath);
}

TEST_CASE("blender_tool: writeToolPrefsText is deterministic and re-parses equal (BT45, AC-9)") {
    ToolPrefs prefs;
    prefs.blenderPath = "/opt/blender/blender";
    const std::string first = writeToolPrefsText(prefs);
    const std::string second = writeToolPrefsText(prefs);
    CHECK(first == second);  // FIXED key order -- non-determinism would reintroduce a per-write diff
    CHECK(first.ends_with("}\n"));
    CHECK(std::count(first.begin(), first.end(), '\n') >= 1);
    CHECK(first.back() == '\n');
    CHECK(first.find("\n\n") == std::string::npos);  // exactly ONE trailing newline, never two
    // "version" is written BEFORE "blenderPath", every time.
    CHECK(first.find("\"version\"") < first.find("\"blenderPath\""));

    const std::optional<ToolPrefs> reparsed = parseToolPrefs(first);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->blenderPath == prefs.blenderPath);
}

TEST_CASE("blender_tool: a wrong tool-preferences version is nullopt, never a repair (BT46, AC-8)") {
    CHECK(TOOL_PREFS_FORMAT_VERSION == 1);  // the version the cases below are written against
    CHECK_FALSE(parseToolPrefs(R"({"version": 99, "blenderPath": "/x"})").has_value());
    CHECK_FALSE(parseToolPrefs(R"({"version": 0, "blenderPath": "/x"})").has_value());
    CHECK_FALSE(parseToolPrefs(R"({"blenderPath": "/x"})").has_value());                  // missing entirely
    CHECK_FALSE(parseToolPrefs(R"({"version": "1", "blenderPath": "/x"})").has_value());  // a string, not an int
}

TEST_CASE("blender_tool: malformed tool-preferences JSON is nullopt and never throws (BT47, AC-8)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TOOL_PREFS_DAMAGED_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK_FALSE(parseToolPrefs(fixture.text).has_value());
    CHECK_FALSE(parseToolPrefs("").has_value());
    CHECK_FALSE(parseToolPrefs("not json at all").has_value());
    CHECK_FALSE(parseToolPrefs("[1, 2, 3]").has_value());  // an array root is not an object root
    CHECK_FALSE(parseToolPrefs("42").has_value());
}

TEST_CASE("blender_tool: a missing blenderPath is an EMPTY value, not a failure (BT48, AC-8)") {
    // The whole purpose of a file that says "the user has not chosen one yet". An absent optional key
    // must never discard a document that is otherwise well-formed.
    const std::optional<ToolPrefs> parsed = parseToolPrefs(R"({"version": 1})");
    REQUIRE(parsed.has_value());
    CHECK(parsed->blenderPath.empty());

    const std::optional<ToolPrefs> emptyValue = parseToolPrefs(R"({"version": 1, "blenderPath": ""})");
    REQUIRE(emptyValue.has_value());
    CHECK(emptyValue->blenderPath.empty());
}

TEST_CASE("blender_tool: a non-string blenderPath is nullopt (BT49, AC-8)") {
    // Distinct from BT48: ABSENT is a default, PRESENT-BUT-WRONG-SHAPE is a document this build did
    // not write, and the two must not be conflated.
    CHECK_FALSE(parseToolPrefs(R"({"version": 1, "blenderPath": 5})").has_value());
    CHECK_FALSE(parseToolPrefs(R"({"version": 1, "blenderPath": null})").has_value());
    CHECK_FALSE(parseToolPrefs(R"({"version": 1, "blenderPath": ["/x"]})").has_value());
}

// ---------------------------------------------------------------------------------------------
// Provenance (BT50-BT58)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: the committed provenance fixture parses to its pinned values (BT50, AC-23)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(PROVENANCE_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<ExportProvenance> parsed = parseExportProvenance(fixture.text);
    REQUIRE(parsed.has_value());
    CHECK(formatGuid(parsed->guid) == FIXTURE_GUID_TEXT);
    CHECK(formatContentHash(parsed->sourceHash) == FIXTURE_SOURCE_HASH_TEXT);
    CHECK(parsed->sourcePath == "props/chair.blend");
    CHECK(parsed->blenderPath == "/Applications/Blender.app/Contents/MacOS/Blender");
    CHECK(parsed->blenderVersion == "5.2.0 LTS");
    CHECK(parsed->scriptVersion == 1);
    CHECK(parsed->settingsFingerprint == FIXTURE_FINGERPRINT_TEXT);
    CHECK(parsed->artifactBytes == 1936);

    // Round-trips: writing what was read and re-reading it yields the same record.
    const std::optional<ExportProvenance> reparsed = parseExportProvenance(writeExportProvenanceText(*parsed));
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->guid == parsed->guid);
    CHECK(reparsed->sourceHash == parsed->sourceHash);
    CHECK(reparsed->scriptVersion == parsed->scriptVersion);
    CHECK(reparsed->settingsFingerprint == parsed->settingsFingerprint);
    CHECK(reparsed->artifactBytes == parsed->artifactBytes);
    CHECK(provenanceMatches(*reparsed, *parsed));
}

TEST_CASE("blender_tool: a wrong provenance version is a MISS, never a repair (BT51, AC-23)") {
    // 3.1.2 D7's policy, and the deliberate INVERSE of .meta's: derived data is disposable, so the
    // whole document is discarded rather than a key being fixed up. Losing it costs one re-conversion.
    const scene_golden::FileBytes fixture = scene_golden::readBytes(PROVENANCE_WRONG_VERSION_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK_FALSE(parseExportProvenance(fixture.text).has_value());
    CHECK(EXPORT_PROVENANCE_FORMAT_VERSION == 1);
}

TEST_CASE("blender_tool: a provenance record missing ANY required key is a miss (BT52, AC-23)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(PROVENANCE_FIXTURE);
    REQUIRE(fixture.ok);
    const std::string whole(fixture.text);

    // Drop each required key in turn by rewriting the document without it. Every one must miss --
    // a record that cannot be fully evaluated is not a record that may serve a cached model.
    for (const std::string_view key :
         {std::string_view("guid"), std::string_view("sourcePath"), std::string_view("sourceHash"),
          std::string_view("blenderPath"), std::string_view("blenderVersion"), std::string_view("scriptVersion"),
          std::string_view("settingsFingerprint"), std::string_view("artifactBytes")}) {
        const std::string quoted = "\"" + std::string(key) + "\"";
        const std::size_t at = whole.find(quoted);
        REQUIRE(at != std::string::npos);
        const std::size_t lineEnd = whole.find('\n', at);
        REQUIRE(lineEnd != std::string::npos);
        std::string without = whole;
        without.erase(at, (lineEnd + 1) - at);
        INFO("dropped key: " << key);
        CHECK_FALSE(parseExportProvenance(without).has_value());
    }
    // Malformed values too: a non-hex guid and a non-hex hash are both misses, never a nil default.
    CHECK_FALSE(parseExportProvenance("not json").has_value());
    CHECK_FALSE(parseExportProvenance("[]").has_value());
}

TEST_CASE("blender_tool: a changed sourceHash invalidates (BT53, AC-23)") {
    const ExportProvenance expected = baselineProvenance();
    ExportProvenance actual = expected;
    CHECK(provenanceMatches(actual, expected));  // the baseline MATCHES -- otherwise this proves nothing
    actual.sourceHash = *parseContentHash("00000000000000000000000000000001");
    CHECK_FALSE(provenanceMatches(actual, expected));
}

TEST_CASE("blender_tool: a changed scriptVersion invalidates (BT54, AC-23)") {
    const ExportProvenance expected = baselineProvenance();
    ExportProvenance actual = expected;
    CHECK(provenanceMatches(actual, expected));
    actual.scriptVersion = expected.scriptVersion + 1;
    CHECK_FALSE(provenanceMatches(actual, expected));
}

TEST_CASE("blender_tool: a changed settingsFingerprint invalidates (BT55, AC-23)") {
    const ExportProvenance expected = baselineProvenance();
    ExportProvenance actual = expected;
    CHECK(provenanceMatches(actual, expected));
    actual.settingsFingerprint = "ffffffffffffffffffffffffffffffff";
    CHECK_FALSE(provenanceMatches(actual, expected));
}

TEST_CASE("blender_tool: a changed blenderVersion invalidates when a version is KNOWN (BT56, AC-23)") {
    const ExportProvenance expected = baselineProvenance();  // blenderVersion NON-empty == known
    REQUIRE_FALSE(expected.blenderVersion.empty());
    ExportProvenance actual = expected;
    CHECK(provenanceMatches(actual, expected));
    actual.blenderVersion = "4.2.1";
    CHECK_FALSE(provenanceMatches(actual, expected));
}

TEST_CASE("blender_tool: sourcePath, blenderPath, guid and artifactBytes are IGNORED (BT57, E24)") {
    // 3.1.2 D11's "keyed by GUID, never by path" one layer up: moving a .blend together with its
    // sidecar, or reinstalling Blender somewhere else, must not throw away a valid conversion. Seed S9
    // adds sourcePath to the comparison and must redden exactly here.
    const ExportProvenance expected = baselineProvenance();
    ExportProvenance actual = expected;
    actual.sourcePath = "somewhere/else/renamed.blend";
    actual.blenderPath = "/completely/different/blender";
    actual.guid = *parseGuid("ffffffffffffffffffffffffffffffff");
    actual.artifactBytes = 999999;
    CHECK(provenanceMatches(actual, expected));  // STILL a hit -- all four are informational
}

TEST_CASE("blender_tool: an EMPTY expected blenderVersion skips that comparison (BT58, AC-22/§A-9)") {
    // The rule that makes AC-22's "zero processes on a cache hit" literally true: on a pure hit
    // nothing has probed a version, so there is none to compare -- and demanding one would require a
    // probe, which is a process. Seed S28 makes the comparison unconditional and must redden here AND
    // in BS31's probeRunCount() == 0 half, two independent failures.
    ExportProvenance expected = baselineProvenance();
    expected.blenderVersion.clear();  // nothing probed yet
    ExportProvenance actual = baselineProvenance();

    actual.blenderVersion = "5.2.0 LTS";
    CHECK(provenanceMatches(actual, expected));
    actual.blenderVersion = "4.2.1";
    CHECK(provenanceMatches(actual, expected));  // ANY actual version matches an unknown expectation
    actual.blenderVersion.clear();
    CHECK(provenanceMatches(actual, expected));

    // ...and the moment a version IS known, the comparison is live again. Both halves in one case, so
    // a seed cannot satisfy one by breaking the other.
    expected.blenderVersion = "5.2.0 LTS";
    actual.blenderVersion = "4.2.1";
    CHECK_FALSE(provenanceMatches(actual, expected));
    actual.blenderVersion = "5.2.0 LTS";
    CHECK(provenanceMatches(actual, expected));
}

// ---------------------------------------------------------------------------------------------
// Export status (BT59-BT62)
// ---------------------------------------------------------------------------------------------

TEST_CASE("blender_tool: an ok status carries its dropped keys and byte count (BT59, AC-32)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(STATUS_OK_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<ExportStatus> parsed = parseExportStatus(fixture.text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->ok);
    CHECK(parsed->blender == "5.2.0 LTS");
    CHECK(parsed->error.empty());
    CHECK(parsed->bytes == 1936);
    // D10's mechanism, observable: an unknown kwarg is DROPPED and REPORTED, and the export still
    // succeeds. This is what lets one script span the whole supported version range.
    REQUIRE(parsed->dropped.size() == 1);
    CHECK(parsed->dropped[0] == "export_bogus_key_that_does_not_exist");
    // "result" is an unknown key to this parser and is ignored rather than rejected -- the script may
    // report more than this build reads.
}

TEST_CASE("blender_tool: a failed status carries the message and ok == false (BT60, AC-33)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(STATUS_FAILED_FIXTURE);
    REQUIRE(fixture.ok);
    const std::optional<ExportStatus> parsed = parseExportStatus(fixture.text);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(parsed->ok);
    CHECK_FALSE(parsed->error.empty());
    CHECK(parsed->error == "the exporter reported ['CANCELLED'] but wrote no usable file");
    CHECK(parsed->bytes == 0);
    CHECK(parsed->dropped.empty());
}

TEST_CASE("blender_tool: damaged status JSON is nullopt and never throws (BT61, E22)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(STATUS_DAMAGED_FIXTURE);
    REQUIRE(fixture.ok);
    CHECK_FALSE(parseExportStatus(fixture.text).has_value());
    CHECK_FALSE(parseExportStatus("").has_value());
    CHECK_FALSE(parseExportStatus("[]").has_value());
    // "ok" is the ONE required field: without it there is no verdict to read.
    CHECK_FALSE(parseExportStatus(R"({"blender": "5.2.0"})").has_value());
    CHECK_FALSE(parseExportStatus(R"({"ok": "true"})").has_value());  // a string, not a bool
}

TEST_CASE("blender_tool: a TRUNCATED status document is nullopt, which the caller reads as absent (BT62, E22)") {
    // The load-bearing case: a killed run can leave a half-written status file, and a partial parse
    // that "recovered" ok == true from it would declare a half-written .glb good. nullopt makes the
    // caller treat it exactly as it treats a missing file.
    const std::string whole = R"({"ok": true, "blender": "5.2.0 LTS", "error": "", "dropped": [], "bytes": 1936})";
    REQUIRE(parseExportStatus(whole).has_value());
    for (std::size_t cut = 1; cut < whole.size(); ++cut) {
        const std::string truncated = whole.substr(0, cut);
        INFO("truncated at byte " << cut);
        CHECK_FALSE(parseExportStatus(truncated).has_value());
    }
}

TEST_CASE("blender_tool: blenderExportDir is empty for an EMPTY project root (BT63, code-review NOTE 6)") {
    // The ONE rule for building <projectRoot>/Library/BlenderExports, and the reason it is a function
    // rather than three concatenations at three call sites: with NO project open the root is empty, and
    // plain concatenation produces "/Library/BlenderExports" -- an absolute path at the FILESYSTEM ROOT
    // that the version probe's own directory creation would then attempt. That fails harmlessly on a
    // POSIX machine and creates a real drive-root directory on Windows, which no CI lane and no macOS
    // run could ever have shown.
    CHECK(blenderExportDir("") == "");
    CHECK(blenderExportDir("/home/me/MyGame") == "/home/me/MyGame/Library/BlenderExports");
    CHECK(blenderExportDir("C:/Users/me/MyGame") == "C:/Users/me/MyGame/Library/BlenderExports");
    // The two names come from the constants, never from a literal spelled a second time.
    CHECK(blenderExportDir("/p").find(std::string(engine::editor::ASSET_CACHE_DIR_NAME)) != std::string::npos);
    CHECK(blenderExportDir("/p").find(std::string(BLENDER_EXPORT_DIR_NAME)) != std::string::npos);
}

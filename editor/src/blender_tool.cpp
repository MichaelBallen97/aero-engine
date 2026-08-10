// Aero Engine — blender_tool.cpp: the PURE half of the Blender CLI integration (task 3.2.4).
// NOTHING HERE LOGS (INV-B10), NOTHING HERE TOUCHES DISK (INV-B14), NOTHING HERE THROWS, and nothing
// here spawns a process (INV-B15 -- every spawn in this task lives in BlenderService::poll()).
// <array> is included EXPLICITLY: modernize-avoid-c-arrays is live on the Linux lane and <array> is
// not transitive on libstdc++ or MSVC (3.1.1's BLOCKING-1).
#include <aero/editor/blender_tool.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

HostOs currentHostOs() noexcept {
#if defined(_WIN32)
    return HostOs::Windows;
#elif defined(__APPLE__)
    return HostOs::MacOs;
#elif defined(__linux__)
    return HostOs::Linux;
#else
    #error "The Aero editor targets macOS, Windows and Linux only (docs/00's platform matrix)."
#endif
}

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse. This TU keeps its OWN copy rather than
// sharing one, matching model_import.cpp's own precedent (which keeps a copy for the same reason).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// The isMetaFileName / isImportableModelName shape: ".blend" ALONE is not a blend file, something
// must precede the extension.
[[nodiscard]] bool endsWithFolded(std::string_view name, std::string_view ext) noexcept {
    if (name.size() <= ext.size()) {
        return false;
    }
    const std::size_t offset = name.size() - ext.size();
    for (std::size_t i = 0; i < ext.size(); ++i) {
        const unsigned char lhs = foldAscii(static_cast<unsigned char>(name[offset + i]));
        const unsigned char rhs = foldAscii(static_cast<unsigned char>(ext[i]));
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::string_view blenderExeName(HostOs os) noexcept {
    return os == HostOs::Windows ? std::string_view("blender.exe") : std::string_view("blender");
}

[[nodiscard]] bool isBlankAscii(std::string_view text) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' && byte != '\v' && byte != '\f') {
            return false;
        }
    }
    return true;
}

// Dedup state: a SORTED std::vector, never a std::set or std::unordered_set (INV-B13). It is a local
// here rather than a member, but the rule is stated once and applied everywhere in this task.
class CandidateSink {
public:
    void add(std::string candidate) {
        if (candidate.empty()) {
            return;
        }
        const auto slot = std::lower_bound(seen.begin(), seen.end(), candidate);
        if (slot != seen.end() && *slot == candidate) {
            return;  // already emitted -- FIRST-SEEN order is what `ordered` preserves
        }
        seen.insert(slot, candidate);
        ordered.push_back(std::move(candidate));
    }
    [[nodiscard]] std::vector<std::string> take() noexcept { return std::move(ordered); }

private:
    std::vector<std::string> seen;     // sorted, for the membership test
    std::vector<std::string> ordered;  // emission order, which is what the caller gets
};

// Windows release lines, DESCENDING -- newest first, so a machine with several installs probes the
// newest one first. Data, not logic (see the header's note): a missing line degrades to "not found
// via this route", and PATH / AERO_BLENDER_PATH / Locate... all bypass it.
constexpr std::array<std::string_view, 13> WINDOWS_RELEASE_LINES = {"5.2", "5.1", "5.0", "4.5", "4.4", "4.3", "4.2",
                                                                    "4.1", "4.0", "3.6", "3.5", "3.4", "3.3"};

constexpr std::array<std::string_view, 2> MACOS_ABSOLUTE_APPS = {
    "/Applications/Blender.app/Contents/MacOS/Blender",
    "/Applications/Blender/Blender.app/Contents/MacOS/Blender",
};

constexpr std::array<std::string_view, 5> LINUX_ABSOLUTE_PATHS = {
    "/usr/bin/blender",     "/usr/local/bin/blender",
    "/snap/bin/blender",    "/var/lib/flatpak/exports/bin/org.blender.Blender",
    "/opt/blender/blender",
};

void appendWindowsWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    // An empty prefix SKIPS the whole family rather than emitting "\Blender Foundation\..." with a
    // missing root -- E2's rule, applied to %PROGRAMFILES% / %LOCALAPPDATA% as well as to $HOME.
    if (!env.programFiles.empty()) {
        for (const std::string_view line : WINDOWS_RELEASE_LINES) {
            sink.add(env.programFiles + "\\Blender Foundation\\Blender " + std::string(line) + "\\blender.exe");
        }
    }
    if (!env.localAppData.empty()) {
        for (const std::string_view line : WINDOWS_RELEASE_LINES) {
            sink.add(env.localAppData + R"(\Programs\Blender Foundation\Blender )" + std::string(line) +
                     "\\blender.exe");
        }
    }
    if (!env.programFiles.empty()) {
        sink.add(env.programFiles + R"(\Steam\steamapps\common\Blender\blender.exe)");
    }
}

void appendMacOsWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    sink.add(std::string(MACOS_ABSOLUTE_APPS[0]));
    if (!env.homeDir.empty()) {
        sink.add(env.homeDir + "/Applications/Blender.app/Contents/MacOS/Blender");
    }
    sink.add(std::string(MACOS_ABSOLUTE_APPS[1]));
}

void appendLinuxWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    // The order is the header's: the three absolute bin paths, the system flatpak export, the per-user
    // flatpak export (skipped entirely when $HOME is unknown), then /opt.
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[0]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[1]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[2]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[3]));
    if (!env.homeDir.empty()) {
        sink.add(env.homeDir + "/.local/share/flatpak/exports/bin/org.blender.Blender");
    }
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[4]));
}

// Digit-by-digit, with an overflow guard. `cursor` is advanced past the digits consumed. Returns
// nullopt when there is no digit at `cursor`, or when the value does not fit in 32 bits.
[[nodiscard]] std::optional<std::uint32_t> parseU32(std::string_view text, std::size_t& cursor) noexcept {
    constexpr std::uint64_t LIMIT = 0xFFFFFFFFULL;
    std::uint64_t value = 0;
    const std::size_t start = cursor;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        value = (value * 10U) + static_cast<std::uint64_t>(text[cursor] - '0');
        if (value > LIMIT) {
            return std::nullopt;  // a number that does not fit is not a version this parser understands
        }
        ++cursor;
    }
    if (cursor == start) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

std::vector<std::string> blenderCandidatePaths(HostOs os, const BlenderEnv& env) {
    CandidateSink sink;

    // 1 and 2 are EXCLUSIVE and terminal (AC-3): a configured path that is wrong must report itself,
    // never fall through to a different Blender's output.
    if (!env.overridePath.empty()) {
        sink.add(env.overridePath);
        return sink.take();
    }
    if (!env.envPath.empty()) {
        sink.add(env.envPath);
        return sink.take();
    }

    const std::string exeName(blenderExeName(os));
    std::size_t scanned = 0;
    for (const std::string& entry : env.pathEntries) {
        if (scanned >= MAX_PATH_ENTRIES_SCANNED) {
            break;  // R3 -- a pathological PATH cannot turn one lazy sweep into thousands of stats
        }
        ++scanned;
        if (entry.empty() || isBlankAscii(entry)) {
            continue;  // E2 -- an empty PATH entry must NEVER be joined into "/blender"
        }
        std::string candidate = entry;  // built with append, not a `+` chain: the Linux lane's
        candidate += '/';               // performance-inefficient-string-concatenation is an error
        candidate += exeName;
        sink.add(std::move(candidate));
    }

    switch (os) {
        case HostOs::Windows:
            appendWindowsWellKnown(env, sink);
            break;
        case HostOs::MacOs:
            appendMacOsWellKnown(env, sink);
            break;
        case HostOs::Linux:
            appendLinuxWellKnown(env, sink);
            break;
    }
    return sink.take();
}

std::optional<BlenderVersion> parseBlenderVersion(std::string_view versionOutput) {
    // FIRST LINE ONLY: the tab-indented "build date:"/"build hash:" continuation lines a real
    // `blender --version` prints must never be reached (§G-4's verbatim capture).
    const std::size_t lineEnd = versionOutput.find('\n');
    std::string_view line = versionOutput.substr(0, lineEnd == std::string_view::npos ? versionOutput.size() : lineEnd);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }

    // The prefix is LITERAL and case-sensitive, and it is at position 0: no leading whitespace is
    // accepted, because a line that does not begin with Blender's own banner is not that banner.
    constexpr std::string_view PREFIX = "Blender ";
    if (line.size() <= PREFIX.size() || line.substr(0, PREFIX.size()) != PREFIX) {
        return std::nullopt;
    }
    std::size_t cursor = PREFIX.size();
    if (line[cursor] < '0' || line[cursor] > '9') {
        return std::nullopt;
    }

    BlenderVersion parsed;
    const std::optional<std::uint32_t> major = parseU32(line, cursor);
    if (!major.has_value()) {
        return std::nullopt;
    }
    parsed.major = *major;
    if (cursor < line.size() && line[cursor] == '.') {
        ++cursor;
        const std::optional<std::uint32_t> minor = parseU32(line, cursor);
        if (!minor.has_value()) {
            return std::nullopt;
        }
        parsed.minor = *minor;
        if (cursor < line.size() && line[cursor] == '.') {
            ++cursor;
            const std::optional<std::uint32_t> patch = parseU32(line, cursor);
            if (!patch.has_value()) {
                return std::nullopt;
            }
            parsed.patch = *patch;
        }
    }
    // Everything after the last parsed number is IGNORED: " LTS" on 5.2, a build suffix elsewhere.
    return parsed;
}

BlenderSupport blenderSupport(const std::optional<BlenderVersion>& version) noexcept {
    if (!version.has_value()) {
        return BlenderSupport::Supported;  // D14/E4 -- attempt, never refuse
    }
    if (*version < BLENDER_ABSOLUTE_MIN) {
        return BlenderSupport::Refused;
    }
    if (*version < BLENDER_MIN_SUPPORTED) {
        return BlenderSupport::Warned;
    }
    return BlenderSupport::Supported;
}

bool isBlendFileName(std::string_view fileName) noexcept { return endsWithFolded(fileName, ".blend"); }

}  // namespace engine::editor

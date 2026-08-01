// Aero Engine — the Project Settings panel's model (task 2.6.2, D3/D7/D8/D9/D11). ImGui-free,
// SDL-free, entt-free, <filesystem>-free, gate-free and SILENT: nothing here logs (INV-S3) and
// nothing here opens a file (INV-S2). It reads the PARSED, in-memory manifest and never re-reads
// project.json (D6).
#include <aero/editor/project_settings.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// The two group titles and the eight labels, named ONCE so the tier-0 battery and the implementation
// cannot drift on a typo. SCREAMING_SNAKE because ConstexprVariableCase is UPPER_CASE and applies to
// locals too (the shell_ui.cpp:28-31 precedent).
constexpr std::string_view GROUP_MANIFEST = "Manifest";
constexpr std::string_view GROUP_LOCATION = "Location";
constexpr std::string_view LABEL_FORMAT_VERSION = "Format version";
constexpr std::string_view LABEL_NAME = "Name";
constexpr std::string_view LABEL_ENGINE_VERSION = "Engine version";
constexpr std::string_view LABEL_LANGUAGE = "Language";
constexpr std::string_view LABEL_ASSETS_PATH = "Assets path";
constexpr std::string_view LABEL_SCENES_PATH = "Scenes path";
constexpr std::string_view LABEL_PROJECT_ROOT = "Project root";
constexpr std::string_view LABEL_MANIFEST_FILE = "Manifest file";

// D9: the HUMAN names, deliberately not languageKey()'s "ts"/"cpp" wire form.
constexpr std::string_view LANGUAGE_TS_DISPLAY = "TypeScript";
constexpr std::string_view LANGUAGE_CPP_DISPLAY = "C++";

constexpr std::string_view BUILD_SUFFIX_OPEN = " (this build: ";
constexpr std::string_view BUILD_SUFFIX_CLOSE = ")";

// D8, and it is the ONLY interpretation this model performs. The predicate is byte-for-byte the one
// the load-time WARN already uses (scene_session.cpp:249): guarded on the BUILD string ONLY, never on
// the manifest's -- so a manifest with an empty engineVersion against a non-empty build DOES get the
// suffix (PS19 pins that arm), and an empty build version suppresses it always (PS17).
[[nodiscard]] std::string engineVersionValue(std::string_view manifestVersion, std::string_view buildVersion) {
    if (buildVersion.empty() || manifestVersion == buildVersion) {
        return std::string(manifestVersion);
    }
    std::string value(manifestVersion);
    value += BUILD_SUFFIX_OPEN;
    value += buildVersion;
    value += BUILD_SUFFIX_CLOSE;
    return value;
}

[[nodiscard]] ProjectSettingsRow makeRow(std::string_view label, std::string value) {
    return ProjectSettingsRow{std::string(label), std::move(value)};
}

}  // namespace

std::string_view languageDisplayName(ProjectLanguage language) noexcept {
    switch (language) {
        case ProjectLanguage::Ts:
            return LANGUAGE_TS_DISPLAY;
        case ProjectLanguage::Cpp:
            return LANGUAGE_CPP_DISPLAY;
    }
    return LANGUAGE_TS_DISPLAY;  // unreachable; every enumerator handled above
}

std::vector<ProjectSettingsGroup> projectSettingsGroups(const ProjectSession& session,
                                                        std::string_view buildEngineVersion) {
    // D11/F6: the LOAD-BEARING guard. manifest() is NOT isOpen()-guarded and close() resets it to a
    // DEFAULT ProjectManifest whose paths are "assets"/"scenes" -- not empty -- so without this the
    // panel would render a plausible, entirely fictional project instead of obviously breaking.
    if (!session.isOpen()) {
        return {};
    }
    const ProjectManifest& manifest = session.manifest();

    ProjectSettingsGroup manifestGroup;
    manifestGroup.title = std::string(GROUP_MANIFEST);
    manifestGroup.rows.reserve(6U);
    // AC-5: from the CONSTANT, never the literal "1" -- a future bump moves this row automatically.
    manifestGroup.rows.push_back(makeRow(LABEL_FORMAT_VERSION, std::to_string(PROJECT_FORMAT_VERSION)));
    manifestGroup.rows.push_back(makeRow(LABEL_NAME, manifest.name));
    manifestGroup.rows.push_back(
        makeRow(LABEL_ENGINE_VERSION, engineVersionValue(manifest.engineVersion, buildEngineVersion)));
    manifestGroup.rows.push_back(makeRow(LABEL_LANGUAGE, std::string(languageDisplayName(manifest.language))));
    // AC-7: VERBATIM. No trailing-separator strip and no join -- that is assetsRoot()'s business, and
    // a hand-edited "assets/" must be visible as "assets/" so a user can see what their file says.
    manifestGroup.rows.push_back(makeRow(LABEL_ASSETS_PATH, manifest.assetsPath));
    manifestGroup.rows.push_back(makeRow(LABEL_SCENES_PATH, manifest.scenesPath));

    ProjectSettingsGroup locationGroup;
    locationGroup.title = std::string(GROUP_LOCATION);
    locationGroup.rows.reserve(2U);
    // AC-12: the session's OWN accessors, never a re-derived concatenation. root() returns a view into
    // a member; it is copied into the row's std::string here, so no view outlives the call (F7).
    locationGroup.rows.push_back(makeRow(LABEL_PROJECT_ROOT, std::string(session.root())));
    locationGroup.rows.push_back(makeRow(LABEL_MANIFEST_FILE, session.manifestPath()));

    std::vector<ProjectSettingsGroup> groups;
    groups.reserve(2U);
    groups.push_back(std::move(manifestGroup));
    groups.push_back(std::move(locationGroup));
    return groups;
}

}  // namespace engine::editor

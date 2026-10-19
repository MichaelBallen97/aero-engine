#pragma once
// Aero Engine -- the Welcome window's and the New Project dialog's chrome as VALUES (task E.6.4, D29). PUBLIC and
// PURE: no ImGui, no <filesystem>, no logging, so tier 0 reaches every decision the two windows draw
// (project_ui_view_test.cpp). The disk is read by project_file.cpp's probeNewProjectTarget, through
// NewProjectForm::probe; the draw is editor/src/project_ui.cpp's. Every length is a ThemeDialogs dp x a SANITISED scale
// (a non-finite or non-positive scale lays out as 1), and every scale parameter is NON-DEFAULTED (US9).
#include <aero/core/math.hpp>
#include <aero/editor/asset_view.hpp>          // TextWidth
#include <aero/editor/editor_glyphs.hpp>       // AERO_GLYPH_MIDDLE_DOT
#include <aero/editor/panel_chrome_model.hpp>  // ChromeRole, ItemRectRecord
#include <aero/editor/project.hpp>             // NewProjectTargetFacts, ProjectLanguage, NEW_PROJECT_LANGUAGE

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// ---- the copy (7-bit literals; non-ASCII only through AERO_GLYPH_*, GL6) -----------------------------------------
inline constexpr std::string_view WELCOME_TITLE = "Aero Editor";
inline constexpr std::string_view WELCOME_HEADLINE = "No project is open.";
inline constexpr std::string_view WELCOME_BLURB = "Create a new project or open an existing one to get started.";
inline constexpr std::string_view WELCOME_NEW_PROJECT_LABEL = "New Project...";    // the File menu's words
inline constexpr std::string_view WELCOME_OPEN_PROJECT_LABEL = "Open Project...";  // the File menu's words
inline constexpr std::string_view WELCOME_RECENTS_HEADING = "RECENT PROJECTS";
inline constexpr std::string_view WELCOME_EMPTY_TITLE = "No recent projects";
inline constexpr std::string_view WELCOME_EMPTY_BODY = "Projects you create or open are listed here.";
inline constexpr std::size_t WELCOME_MAX_VISIBLE_CARDS = 5;  // past it the list scrolls (D6)

inline constexpr std::string_view NEW_PROJECT_TITLE = "New project";
inline constexpr std::string_view NEW_PROJECT_NAME_LABEL = "Project name";
inline constexpr std::string_view NEW_PROJECT_NAME_HINT = "MyGame";
inline constexpr std::string_view NEW_PROJECT_LOCATION_LABEL = "Location";
inline constexpr std::string_view NEW_PROJECT_NO_LOCATION_TEXT = "no folder chosen";
inline constexpr std::string_view NEW_PROJECT_BROWSE_LABEL = "Browse...";
inline constexpr std::string_view NEW_PROJECT_SETTINGS_HEADING = "SETTINGS";
inline constexpr std::string_view NEW_PROJECT_LANGUAGE_LABEL = "Scripting language";
// D17: what Create writes, and ADR-008's rule; the mock's "both can coexist" contradicts it and never ships.
inline constexpr std::string_view NEW_PROJECT_LANGUAGE_HINT =
    "New projects are created as TypeScript -- the language is fixed at creation";
// D18: the 2.1.3 / E.6.2 convention -- a control a later task owns ships disabled with a tooltip naming it.
// docs/tasks/phase-4.md's 4.6.1 lists the creation dialog's language choice, which is what makes it true.
inline constexpr std::string_view LANGUAGE_CONTROL_TOOLTIP = "Not implemented yet -- task 4.6.1";
inline constexpr std::string_view NEW_PROJECT_CANCEL_LABEL = "Cancel";
inline constexpr std::string_view NEW_PROJECT_CREATE_LABEL = "Create project";

// The validation line's words (D10). Every " . " separator is a middle dot.
inline constexpr std::string_view NEW_PROJECT_LEAD_CREATES = "creates ";
inline constexpr std::string_view NEW_PROJECT_HINT_NO_NAME = "enter a project name";
inline constexpr std::string_view NEW_PROJECT_HINT_NO_LOCATION = "choose a folder with Browse...";
inline constexpr std::string_view NEW_PROJECT_HINT_CHECKING = "checking...";
inline constexpr std::string_view NEW_PROJECT_TAIL_NEW_FOLDER = " " AERO_GLYPH_MIDDLE_DOT " new folder";
inline constexpr std::string_view NEW_PROJECT_TAIL_EMPTY_FOLDER = " " AERO_GLYPH_MIDDLE_DOT " folder is empty";
inline constexpr std::string_view NEW_PROJECT_TAIL_LOCATION_MISSING =
    " " AERO_GLYPH_MIDDLE_DOT " the folder does not exist";
inline constexpr std::string_view NEW_PROJECT_TAIL_TARGET_IS_FILE =
    " " AERO_GLYPH_MIDDLE_DOT " a file already exists there";
inline constexpr std::string_view NEW_PROJECT_TAIL_TARGET_NOT_EMPTY =
    " " AERO_GLYPH_MIDDLE_DOT " the folder is not empty";

// ---- the scripting-language control (D18) --------------------------------------------------------------------------
struct LanguageCard {
    ProjectLanguage language = ProjectLanguage::Ts;
    std::string_view title;
    std::string_view subtitle;  // what the choice MEANS under ADR-008's table, never a feature's availability (D18)
};
inline constexpr std::array<LanguageCard, 2> NEW_PROJECT_LANGUAGE_CARDS{{
    {ProjectLanguage::Cpp, "C++", "Native code, built for each platform."},
    {ProjectLanguage::Ts, "TypeScript", "Embedded scripts, no toolchain to export."},
}};
// The card drawn selected: the language Create writes (NEW_PROJECT_LANGUAGE), never a stored choice.
[[nodiscard]] bool languageCardSelected(ProjectLanguage language) noexcept;
[[nodiscard]] std::string_view languageCardTitle(ProjectLanguage language) noexcept;

// ---- the info box (D16): exactly what createProject writes --------------------------------------------------------
struct InfoSpan {
    std::string_view text;
    bool code = false;  // a file name: Mono, textSecondary; else prose: BodySmall, textLabel
};
inline constexpr std::array<InfoSpan, 7> NEW_PROJECT_INFO_SPANS{{
    {"Aero writes ", false},
    {"project.json", true},
    {" and creates the ", false},
    {"assets/", true},
    {" and ", false},
    {"scenes/", true},
    {" folders.", false},
}};

// ---- the validation line (D10, D11) -----------------------------------------------------------------------------
// The first state that matches wins, in this order -- createProject's own refusal order.
enum class NewProjectTargetState : std::uint8_t {
    NoName = 0,         // validateProjectName == Empty
    BadName,            // any other NameProblem
    NoLocation,         // the location is empty
    Checking,           // no probe answers THIS location yet: untouched facts, or facts for another location
    LocationMissing,    // the location is not a directory
    TargetIsFile,       // a file sits where the folder would go
    TargetNotEmpty,     // a non-empty folder sits there
    CreatesFolder,      // the folder will be created
    AdoptsEmptyFolder,  // an empty folder sits there and is adopted (E7/AC-13)
};
inline constexpr std::size_t NEW_PROJECT_TARGET_STATE_COUNT = 9;
// Lexical states from the CURRENT inputs; filesystem states from `facts` -- even when facts.name trails the current
// name by the one frame the probe lags (D12), so Create does not grey on every keystroke.
[[nodiscard]] NewProjectTargetState classifyNewProjectTarget(std::string_view name, std::string_view location,
                                                             const NewProjectTargetFacts& facts) noexcept;
// THE one predicate of the Create button, its Enter binding and its GPU seam: CreatesFolder or AdoptsEmptyFolder.
[[nodiscard]] bool newProjectCreateEnabled(NewProjectTargetState state) noexcept;

struct LineSpan {
    std::string text;
    ChromeRole role = ChromeRole::TextMuted;
};
struct NewProjectLine {
    LineSpan lead;  // "creates "
    LineSpan path;  // the home-abbreviated target (or location); "" when the state names no path
    LineSpan tail;  // the reason, the hint or the " . new folder" note
};
// The line for `state`. `home` abbreviates the path (abbreviateHome); the target is facts.targetPath, never re-joined.
// A non-empty `createError` replaces the line, role Error, ONLY on the two creatable states (D11).
[[nodiscard]] NewProjectLine newProjectLine(NewProjectTargetState state, std::string_view name,
                                            std::string_view location, const NewProjectTargetFacts& facts,
                                            std::string_view createError, std::string_view home);
// Fits the line into `available`: the path left-elided first, then -- with no room for its ellipsis -- the tail
// right-elided, then the lead. Every cut lands on a code-point boundary.
[[nodiscard]] NewProjectLine fitNewProjectLine(const NewProjectLine& line, float available, const TextWidth& width);
// elidePathLeft / elideCaptionRight behind the gate status_bar.hpp tells every caller to apply: the text when it fits,
// "" when `budget` cannot hold the ellipsis itself, else the elided text, which then fits.
[[nodiscard]] std::string fitPathLeft(std::string_view path, float budget, const TextWidth& width);
[[nodiscard]] std::string fitTextRight(std::string_view text, float budget, const TextWidth& width);
// The footer's summary of today's fields (D19): "TypeScript", or "<name> . TypeScript" -- the name verbatim.
[[nodiscard]] std::string newProjectFooterSummary(std::string_view name);

// ---- the shared header strip and the mark (D5, D26) ----------------------------------------------------------------
// The mark's four corners -- top, right, bottom, left -- a markSide square turned 45 degrees around `center`.
[[nodiscard]] std::array<Vec2, 4> aeroMarkPoints(Vec2 center, float uiScale) noexcept;
struct DialogHeaderLayout {  // every point window-relative
    float height = 0.0F;
    Vec2 markCenter{};
    float titleX = 0.0F;   // the title's pen
    float centerY = 0.0F;  // the strip's mid-line: each text is centred on it
    float versionRight = 0.0F;
    bool closeDrawn = false;
    Vec2 closeMin{};
    Vec2 closeMax{};
    friend constexpr bool operator==(const DialogHeaderLayout&, const DialogHeaderLayout&) = default;
};
[[nodiscard]] DialogHeaderLayout dialogHeaderLayout(float width, bool closeButton, float uiScale) noexcept;

// ---- the New Project dialog's layout (D2, D15) -------------------------------------------------------------------
struct NewProjectDialogLayout {  // points; x window-relative
    float width = 0.0F;  // newProjectWidth, clamped to the work area less screenMargin a side, floored at minWidth
    float paddingX = 0.0F;
    float contentWidth = 0.0F;  // width - 2 x paddingX
    float headerHeight = 0.0F;
    float footerHeight = 0.0F;
    float windowRounding = 0.0F;
    float controlRounding = 0.0F;
    Vec2 fieldPadding{};   // (fieldPaddingX, (fieldHeight - body points) / 2): a frame lands on fieldHeight
    Vec2 buttonPadding{};  // (buttonPaddingX, (buttonHeight - body points) / 2)
    float fieldHeight = 0.0F;
    float buttonHeight = 0.0F;
    float browseGap = 0.0F;
    float bodyTop = 0.0F;
    float labelGap = 0.0F;
    float fieldGap = 0.0F;
    float lineGap = 0.0F;
    float dividerGap = 0.0F;
    float headingGap = 0.0F;
    float cardWidth = 0.0F;  // (contentWidth - cardGap) / 2
    float cardHeight = 0.0F;
    float cardGap = 0.0F;
    float radioInset = 0.0F;
    float radioDiameter = 0.0F;
    float radioDot = 0.0F;
    float cardTextInset = 0.0F;
    float cardTitleTop = 0.0F;
    float cardLineGap = 0.0F;
    float hintGap = 0.0F;
    float infoGap = 0.0F;
    float infoHeight = 0.0F;
    float infoDotInset = 0.0F;
    float infoDotDiameter = 0.0F;
    float infoTextInset = 0.0F;
    float footerButtonGap = 0.0F;
    friend constexpr bool operator==(const NewProjectDialogLayout&, const NewProjectDialogLayout&) = default;
};
[[nodiscard]] NewProjectDialogLayout newProjectDialogLayout(float workWidth, float uiScale) noexcept;

struct NewProjectFooterLayout {  // x window-relative, y footer-relative
    float summaryX = 0.0F;
    float summaryMaxWidth = 0.0F;  // ends footerButtonGap before Cancel; never negative
    float cancelX = 0.0F;
    float createX = 0.0F;  // its right edge is width - paddingX
    float buttonY = 0.0F;
    friend constexpr bool operator==(const NewProjectFooterLayout&, const NewProjectFooterLayout&) = default;
};
// The two widths are the buttons' own (ImGui::Button's label width + 2 x buttonPadding.x, measured by the draw).
[[nodiscard]] NewProjectFooterLayout newProjectFooterLayout(const NewProjectDialogLayout& layout, float cancelWidth,
                                                            float createWidth) noexcept;

// ---- the Welcome window (D6, D9, D34) ---------------------------------------------------------------------------
// The blurb's height wrapped at `wrapWidth`, as the draw measures it under the Body face (project_ui.cpp's
// bodyWrappedHeight); tier 0 passes arithmetic. welcomeLayout asks it at its OWN content width (D34).
using WrappedTextHeight = std::function<float(float wrapWidth)>;

struct RecentCardText {
    std::string title;  // rootDisplayName(root): the folder name
    std::string path;   // abbreviateHome(root, home), unelided
};
[[nodiscard]] RecentCardText recentCardText(std::string_view root, std::string_view home);

struct WelcomeLayout {   // points; x window-relative
    float width = 0.0F;  // welcomeWidth, clamped like the dialog's
    float paddingX = 0.0F;
    float contentWidth = 0.0F;
    float headerHeight = 0.0F;
    float windowRounding = 0.0F;
    float controlRounding = 0.0F;
    Vec2 buttonPadding{};
    float buttonHeight = 0.0F;
    float buttonGap = 0.0F;  // footerButtonGap
    float bodyTop = 0.0F;
    float sectionGap = 0.0F;
    float labelGap = 0.0F;
    float cardHeight = 0.0F;
    float cardGap = 0.0F;
    float cardTitleTop = 0.0F;
    float cardLineGap = 0.0F;
    float iconInset = 0.0F;
    float textInset = 0.0F;
    float blurbHeight = 0.0F;       // the blurb as wrapped at contentWidth: never less than one Body line (D34)
    std::size_t visibleCards = 0;   // 0 with no recents; else in [1, min(count, WELCOME_MAX_VISIBLE_CARDS)]
    bool listScrolls = false;       // more recents than visibleCards
    float listHeight = 0.0F;        // visibleCards cards and the gaps between them
    float emptyStateHeight = 0.0F;  // the well: folded to the room, in [emptyStateBlock, emptyStateHeight dp] (D34)
    float emptyStateBlock = 0.0F;   // the well's icon, title and body and their two gaps: it never folds below this
    friend constexpr bool operator==(const WelcomeLayout&, const WelcomeLayout&) = default;
};
// The window FOLDS before it overflows the work area (D6, D34): the list to as many cards as fit under the fixed
// rows -- the blurb counted at the height `blurbHeight` answers for the content width -- never fewer than one, never
// more than WELCOME_MAX_VISIBLE_CARDS; with no recents, the well to the room left, never below its own block. An
// empty, non-finite or short measure counts one Body line.
[[nodiscard]] WelcomeLayout welcomeLayout(std::size_t recentCount, Vec2 workSize, const WrappedTextHeight& blurbHeight,
                                          float uiScale);

// ---- what the two windows DREW, for the GPU tier (D27) -- the ShellChromeRecord shape ------------------------------
// Filled by editor/src/project_ui.cpp FROM WHAT ImGui DID: window and item rects, Disabled flags from GetItemFlags, the
// strings handed to AddText written where each is drawn, the work area from GetMainViewport(). The layout and the
// state are the stated inputs the draw used; nothing here is a request read back.
// The shared header strip, filled by drawDialogHeader itself: the title as drawn, the version ONLY when it drew, and
// the close box's rect and Disabled flag ONLY when it drew (an undrawn box keeps the default, enabled == false).
struct DialogHeaderRecord {
    std::string title;
    std::string version;
    ItemRectRecord close{};
};
struct RecentCardRecord {
    std::string title;  // as handed to AddText
    std::string path;   // as handed to AddText: abbreviated, then fitted
    Vec2 min{};         // the card's InvisibleButton
    Vec2 max{};
};
struct WelcomeRecord {
    std::uint64_t framesDrawn = 0;  // + 1 per frame its Begin returned true: every read below is vacuous while 0
    bool drawnThisFrame = false;
    float uiScaleAtDraw = 0.0F;  // currentUiScale() as it drew (named apart from US9's token)
    Vec2 windowPos{};            // GetWindowPos / GetWindowSize inside the body
    Vec2 windowSize{};
    Vec2 workPos{};  // GetMainViewport()'s work area
    Vec2 workSize{};
    float scrollMaxX = 0.0F;    // GetScrollMaxX(): how far ImGui would let the window scroll sideways
    bool scrollbarY = false;    // it drew its vertical scrollbar, narrowing the inner rect (clamped, D34)
    WelcomeLayout layout{};     // the layout's answer, as used
    DialogHeaderRecord header;  // what the header strip drew (no close box on the Welcome)
    bool emptyStateDrawn = false;
    float listHeight = 0.0F;  // the recents child's height, as ImGui laid it out (GetWindowSize inside it)
    std::vector<RecentCardRecord> cards;
    ItemRectRecord newProject{};
    ItemRectRecord openProject{};
};
struct NewProjectRecord {
    std::uint64_t framesDrawn = 0;  // + 1 per frame BeginPopupModal returned true
    bool popupOpenAtEntry = false;  // IsPopupOpen(the modal's id), read FIRST on every call: false once answered
    bool drawnThisFrame = false;
    float uiScaleAtDraw = 0.0F;
    Vec2 windowPos{};
    Vec2 windowSize{};
    Vec2 workPos{};
    Vec2 workSize{};
    float scrollMaxX = 0.0F;  // as the Welcome's
    bool scrollbarY = false;
    NewProjectDialogLayout layout{};
    DialogHeaderRecord header;                                    // title, version and the close box, as drawn
    NewProjectTargetState state = NewProjectTargetState::NoName;  // the classification the draw used
    NewProjectLine line{};                                        // as drawn: after the fit
    std::string location;  // the Location box's text as drawn; "" when the placeholder drew
    std::string summary;   // the footer summary as drawn
    ItemRectRecord nameField{};
    ItemRectRecord locationField{};
    ItemRectRecord browse{};
    ItemRectRecord cancel{};
    ItemRectRecord create{};
    std::array<ItemRectRecord, 2> language{};  // NEW_PROJECT_LANGUAGE_CARDS order; drawnActive = drawn selected
};
struct ProjectUiRecord {
    WelcomeRecord welcome;
    NewProjectRecord newProject;
};
// Open one frame of a window's record: the monotonic framesDrawn kept, every per-frame field reset (capacity kept).
// Called as the window function's FIRST statement, before any early return.
void beginWelcomeRecordFrame(WelcomeRecord& record) noexcept;
void beginNewProjectRecordFrame(NewProjectRecord& record) noexcept;

// ---- the GPU tier's click-equivalents (D27) ----------------------------------------------------------------------
// The pending input lands in form.name / form.location before the draw; the dialog then reloads the ACTIVE Name
// widget from form.name on that one tick (D27), because an active InputText owns its text.
struct NewProjectInput {  // exactly what the Name widget and a ProjectLocation dialog result write
    std::string name;
    std::string location;
};
enum class NewProjectAnswer : std::uint8_t { None = 0, Create, Dismiss };

}  // namespace engine::editor

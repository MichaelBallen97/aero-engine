#pragma once
// Aero Engine — the editor's context router (task E.3.2). PUBLIC, PURE, ImGui-FREE, World-FREE and
// session-FREE: it names no panel class, no World, no AssetDatabase and no ImGui type, which is what
// lets the tier-0 shell test drive the whole matrix with no window and no GPU.
//
// THE SPLIT, and it is the design (D13): EditorApp DETECTS (it owns the Selection and both sessions,
// and it is ImGui-free by rule -- 2.1.3 D1), shell_ui.cpp APPLIES (it is the ImGui frame-composition
// TU and owns the one focus slot), and the two pure functions below DECIDE. Nothing here derives which
// panel a PATH belongs to: the router reads the answer each session already wrote (D1). In particular
// it spells NEITHER isImportableModelName NOR isBlendFileName -- it reads ModelImportSession's own
// SessionState, one tick later, and that one tick is the whole price of having no second copy of a
// predicate that would go stale the day a ninth extension lands.
//
// NO toString, EVER. doctest's DOCTEST_STRINGIFY expands to an UNQUALIFIED toString(...), so a
// toString on a type declared here would be found by ADL, beat doctest's own template and hard-error
// inside doctest.h. A label function, if one is ever wanted, is named anything else
// (audioClipLoadStatusLabel is the precedent) -- and nothing wants one: neither enum below is logged
// or serialized.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::editor {

// WHAT RAISED A PANEL. THE NUMERIC ORDER **IS** THE PRIORITY (D17): when more than one source is
// observed in a single tick the HIGHEST value wins, so the outcome does not depend on the order
// EditorApp happens to call the observe* functions -- which is the point, because a reordering of
// that block would otherwise silently change behaviour.
//
// This enum is NEVER serialized and NEVER logged numerically, so -- unlike AssetBrowserPanel's
// ActionKind and ModelImportSession's SessionState, which are both APPEND-ONLY -- a new source is
// INSERTED at the position its priority demands, not appended. Re-read this sentence before adding
// one, and re-read RT27, which pins every value.
// performance-enum-size: the explicit underlying type is mandatory, like every engine enum.
enum class RouteSource : std::uint8_t {
    None = 0,
    ImportableAsset,  // the browser's selection settled on a file some importer claims
    MaterialAsset,    // the material session retargeted (its OWN sticky rule, never re-derived)
    EntitySelection,  // the scene selection was operated on and is non-empty
};

// The roadmap's "source of selection -> panel id", and the whole of it. TOTAL and PURE.
// Returns a STRING LITERAL: static lifetime, exactly Panel::id()'s own contract, so a caller may hand
// it straight to ImGui::SetWindowFocus with no allocation and no lifetime question. "" for None.
//
// Each id is FROZEN, because it is the ImGui window name AND the imgui.ini settings key
// (panel.hpp:46-48, "RENAMING AN ID ORPHANS EVERY USER'S SAVED LAYOUT FOR THAT PANEL") -- these three
// strings must stay byte-identical to InspectorPanel::id(), MaterialPanel::id() and
// ImportDetailsPanel::id() forever. I159 pins that they do, by reading those three files' source text.
[[nodiscard]] const char* routedPanelId(RouteSource source) noexcept;

// Everything that can stop a latched route from being applied. NAMED BOOLEANS, never an ImGui flag --
// an ImGui type here would make every consumer of this header ImGui-aware (panel.hpp's PanelOptions
// rule, stated in those words at panel.hpp:25-26). Defaults are the PERMISSIVE reading, so a
// partially-filled aggregate in a test says "allow" and each case names only the guard it is about.
struct RouteGuards {
    bool enabled = true;                  // the user preference (View > Focus Follows Selection)
    bool sourceStillValid = true;         // the thing to show still exists (D6)
    bool targetAvailable = true;          // the panel is registered AND visible (D7)
    bool explicitFocusThisFrame = false;  // requestPanelFocus won this frame (D8)
    bool textInputActive = false;         // io.WantTextInput -- FocusWindow STEALS the active widget
    bool dragPayloadLive = false;         // GetDragDropPayload() != nullptr (the PUBLIC drag signal)
    bool gizmoDragActive = false;         // ImGuizmo::IsUsing()
    bool popupOpen = false;               // IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)
};

enum class RouteOutcome : std::uint8_t {
    Drop = 0,  // terminal: the caller CLEARS the latch
    Hold,      // transient: the caller KEEPS the latch and re-evaluates next tick
    Apply,     // raise the panel, then clear the latch
};

// THE ORDER OF THE TESTS IS PART OF THE CONTRACT (D5): every DROP condition is tested BEFORE every
// HOLD condition, so a route that can never become valid is never held forever. RT9 pins it -- a
// DISABLED route with a live drag is Drop, not Hold. Each of the four HOLD conditions is a transient
// ImGui state that ends on a mouse-up, a click-away or an Escape; each of the four non-trivial DROP
// conditions can persist indefinitely.
//
// NAME SHADOWING, deliberate and safe: ShellUiState has a `routeOutcome` MEMBER carrying this
// function's result. `state.routeOutcome = routeOutcome(state.routeSource, guards);` is well-formed --
// the unqualified name finds this function, the member is reachable only through `state.`. Never call
// this from a member function of a type that has a routeOutcome member: member lookup would win.
[[nodiscard]] RouteOutcome routeOutcome(RouteSource source, const RouteGuards& guards) noexcept;

// THE LATCH. One slot, highest priority wins, cleared only by the caller. Holds VALUES ONLY: no World,
// no session, no AssetDatabase, no filesystem -- so EditorApp's own noexcept move survives it (F15),
// which the two static_asserts below hold structurally.
class ContextRouter {
public:
    // OFF drops any pending route immediately: a held route that fired the moment the preference came
    // back on would be the exact focus-steal this switch exists to prevent.
    void setEnabled(bool on) noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabledValue; }

    // ---- the three observations. Each is a RECONCILE: compare, then latch only on a real change. --
    // Called unconditionally every tick from EditorApp's reconcile block, in ANY order (D17).
    // Every one of them is a NO-OP while enabled() is false -- nothing latches, and no baseline moves
    // either, so turning the preference back on does not fire a route for something the user did
    // while it was off.
    //
    // Each takes its fact BY VALUE or as a string_view and stores a std::string. A fixed buffer was
    // rejected rather than merely not chosen: a cap makes two deeply-nested sibling paths compare
    // EQUAL past it, which silently DROPS a route with no log, no assertion and no wrong picture.

    // `revision` is Selection::revision() -- a count of selection OPERATIONS, not a diff (D2).
    // A bump with an EMPTY selection ADVANCES THE BASELINE and latches nothing (D18), which is what
    // makes clear(), newScene, openSceneFile and a project swap silent by construction.
    void observeEntitySelection(std::uint64_t revision, bool nonEmpty);

    // `targetPath` is MaterialSession::targetPath(): "" when Untargeted. A change TO "" advances the
    // baseline and latches nothing; a later change back to the same path latches again.
    void observeMaterialTarget(std::string_view targetPath);

    // `settled` is `ModelImportSession::state() != SessionState::Idle` and `claimed` is
    // `!= SessionState::NotImportable`. WHEN `settled` IS FALSE THIS FUNCTION DOES NOTHING AT ALL --
    // it does not even advance the baseline -- because setTarget() sets Idle synchronously on a path
    // change (model_import_session.cpp:88) while service() classifies in the POST-DRAW slot
    // (editor_app.cpp:1000), so the honest answer is one tick away (D4). That one extra tick is what
    // buys "the router duplicates no predicate". RT18 and I155 are that gate's own proofs.
    void observeImportTarget(std::string_view targetPath, bool settled, bool claimed);

    [[nodiscard]] RouteSource pending() const noexcept { return pendingSource; }
    void clearPending() noexcept { pendingSource = RouteSource::None; }

    // A LIFETIME counter, deliberately never reset: without it "no route was latched" and "the router
    // did nothing observable" are indistinguishable (MaterialSession::writeCount()'s own reasoning).
    // It counts LATCHES, not observe calls -- a second observation of the SAME source in one tick does
    // not move it, which RT15 pins.
    [[nodiscard]] std::size_t latchCount() const noexcept { return latches; }

private:
    void latch(RouteSource source) noexcept;  // keeps the HIGHER priority

    bool enabledValue = true;
    // Both baselines start at the value their source starts at -- Selection::revision() starts at 0
    // and both sessions start untargeted -- so the FIRST tick observes no change and latches nothing.
    // That is also what makes startup silent: EditorApp::create can open a project, which runs
    // newScene -> resetSceneState -> selection.clear(), and an empty selection latches nothing anyway.
    std::uint64_t lastRevision = 0;
    std::string lastMaterialTarget;
    std::string lastImportTarget;
    RouteSource pendingSource = RouteSource::None;
    std::size_t latches = 0;
};

static_assert(std::is_nothrow_move_constructible_v<ContextRouter>);
static_assert(std::is_nothrow_move_assignable_v<ContextRouter>);

}  // namespace engine::editor

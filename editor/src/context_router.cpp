// Aero Engine — the context router's out-of-line half (task E.3.2). PURE std C++ and nothing else: no
// ImGui, no SDL, no entt, no logging, no filesystem. Every function here is total.
#include <aero/editor/context_router.hpp>

#include <cstdint>
#include <string_view>

namespace engine::editor {

const char* routedPanelId(RouteSource source) noexcept {
    // STRING LITERALS: static lifetime, and byte-identical to the three panels' own id() returns --
    // InspectorPanel::id() (editor/src/inspector_panel.hpp:28), MaterialPanel::id()
    // (editor/src/material_panel.hpp:54) and ImportDetailsPanel::id()
    // (editor/src/import_details_panel.hpp:23). I159 reads all four files and compares them.
    //
    // A SWITCH WITH NO default:, on purpose -- adding an enumerator is then a -Wswitch warning at this
    // exact line rather than a silent fall through to "".
    switch (source) {
        case RouteSource::EntitySelection:
            return "Inspector";
        case RouteSource::MaterialAsset:
            return "Material";
        case RouteSource::ImportableAsset:
            return "Import Details";
        case RouteSource::None:
            break;
    }
    return "";
}

RouteOutcome routeOutcome(RouteSource source, const RouteGuards& guards) noexcept {
    // ---- TERMINAL, in this order (D5). NEVER reorder a Drop below a Hold: each of these four can
    // persist indefinitely, so holding on one is holding forever. ----
    if (source == RouteSource::None) {
        return RouteOutcome::Drop;  // nothing pending -- the common case, every frame
    }
    if (!guards.enabled) {
        return RouteOutcome::Drop;  // a held route would fire the moment the preference came back on
    }
    if (!guards.sourceStillValid) {
        return RouteOutcome::Drop;  // the thing to show is gone (D6)
    }
    if (!guards.targetAvailable) {
        return RouteOutcome::Drop;  // unregistered, or hidden -- and it may stay hidden forever (D7)
    }
    if (guards.explicitFocusThisFrame) {
        return RouteOutcome::Drop;  // a command outranks an inference (D8)
    }
    // ---- TRANSIENT: every one of these ends on a mouse-up, a click-away or an Escape, so the latch
    // survives and is re-evaluated next tick rather than being discarded. ----
    if (guards.textInputActive || guards.dragPayloadLive || guards.gizmoDragActive || guards.popupOpen) {
        return RouteOutcome::Hold;
    }
    return RouteOutcome::Apply;
}

void ContextRouter::setEnabled(bool on) noexcept {
    enabledValue = on;
    if (!on) {
        pendingSource = RouteSource::None;  // OFF empties the latch; it never queues behind the switch
    }
}

void ContextRouter::latch(RouteSource source) noexcept {
    // <=, not <: a SECOND observation of the SAME source in one tick is not a second latch, so
    // latchCount() counts ticks-that-routed rather than observe calls (RT15).
    if (static_cast<std::uint8_t>(source) <= static_cast<std::uint8_t>(pendingSource)) {
        return;
    }
    pendingSource = source;
    ++latches;
}

// ---- THE BASELINE ADVANCES WHILE DISABLED; ONLY THE LATCH IS GATED. ------------------------------
// `enabledValue` appears in the LATCH condition of all three, and in the early return of NONE of them,
// and that asymmetry is the whole contract. Gating the early return instead leaves a STALE baseline
// behind, so the first observation after the preference is turned back on compares this tick's value
// against one from before it was turned off, sees a "change", and raises a panel for something the
// user did minutes ago -- a focus steal triggered by ticking a menu item, which is the exact behaviour
// this task exists to remove. Reachable in three clicks: routing off, click an entity, tick
// View > Focus Follows Selection back on.
//
// An observation made while disabled is therefore SEEN and FORGOTTEN, never SKIPPED. RT24 drives it.

void ContextRouter::observeEntitySelection(std::uint64_t revision, bool nonEmpty) {
    if (revision == lastRevision) {
        return;
    }
    lastRevision = revision;
    if (enabledValue && nonEmpty) {
        latch(RouteSource::EntitySelection);
    }
}

void ContextRouter::observeMaterialTarget(std::string_view targetPath) {
    if (targetPath == lastMaterialTarget) {
        return;
    }
    lastMaterialTarget = targetPath;
    if (enabledValue && !targetPath.empty()) {
        latch(RouteSource::MaterialAsset);
    }
}

void ContextRouter::observeImportTarget(std::string_view targetPath, bool settled, bool claimed) {
    // `!settled` STAYS IN THE EARLY RETURN, and it is the one place the rule above does not apply: an
    // UNSETTLED tick must leave the BASELINE alone too, or the change is consumed by setTarget()'s Idle
    // frame and never seen again (D4). That rule is about the SESSION's state machine and is wholly
    // independent of the user preference, so `enabledValue` must not join it here -- RT18 and I155 are
    // what hold that line.
    if (!settled || targetPath == lastImportTarget) {
        return;
    }
    lastImportTarget = targetPath;
    if (enabledValue && !targetPath.empty() && claimed) {
        latch(RouteSource::ImportableAsset);
    }
}

}  // namespace engine::editor

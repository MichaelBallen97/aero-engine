// Aero Engine — the context router's pure decision and its latch (task E.3.2, RT1-RT27). Tier 0:
// aero_editor_shell_test, no ImGui, no GPU, no window, no World. Every case here is a statement about
// context_router.hpp alone.
#include <aero/editor/context_router.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs std::string_view
#include <string_view>
#include <type_traits>
#include <utility>

using engine::editor::ContextRouter;
using engine::editor::routedPanelId;
using engine::editor::RouteGuards;
using engine::editor::RouteOutcome;
using engine::editor::routeOutcome;
using engine::editor::RouteSource;

namespace {

// The three frozen ids, spelled here as the test's own expectation rather than read from the router:
// a test that compared routedPanelId's output against routedPanelId's own constant would assert
// nothing (E.1.2's GR8 lesson -- read the value under test OFF the thing under test).
constexpr std::string_view INSPECTOR_ID = "Inspector";
constexpr std::string_view MATERIAL_ID = "Material";
constexpr std::string_view IMPORT_DETAILS_ID = "Import Details";

}  // namespace

TEST_CASE("editor: routedPanelId is total and names the three frozen panel ids (RT1)") {
    // Compared against the test's OWN literals, never against a second call to the function under
    // test -- E.1.2's GR8 lesson: a case that reads both sides off the same source asserts nothing.
    CHECK(std::string_view(routedPanelId(RouteSource::EntitySelection)) == INSPECTOR_ID);
    CHECK(std::string_view(routedPanelId(RouteSource::MaterialAsset)) == MATERIAL_ID);
    CHECK(std::string_view(routedPanelId(RouteSource::ImportableAsset)) == IMPORT_DETAILS_ID);
    CHECK(std::string_view(routedPanelId(RouteSource::None)).empty());
    // TOTAL: the three real sources never return "" -- which is what a missing switch arm would do.
    CHECK_FALSE(std::string_view(routedPanelId(RouteSource::EntitySelection)).empty());
    CHECK_FALSE(std::string_view(routedPanelId(RouteSource::MaterialAsset)).empty());
    CHECK_FALSE(std::string_view(routedPanelId(RouteSource::ImportableAsset)).empty());
}

TEST_CASE("editor: routedPanelId returns a STATIC-lifetime pointer, twice the same address (RT2)") {
    // The contract a caller relies on when it hands the result straight to ImGui::SetWindowFocus with
    // no copy. A std::string(...).c_str() implementation would dangle, and ASan on the Debug lane
    // would catch the read -- but the address comparison catches it on every lane.
    static_assert(noexcept(routedPanelId(RouteSource::None)));
    CHECK(routedPanelId(RouteSource::EntitySelection) == routedPanelId(RouteSource::EntitySelection));
    CHECK(routedPanelId(RouteSource::MaterialAsset) == routedPanelId(RouteSource::MaterialAsset));
    CHECK(routedPanelId(RouteSource::ImportableAsset) == routedPanelId(RouteSource::ImportableAsset));
    CHECK(routedPanelId(RouteSource::None) == routedPanelId(RouteSource::None));
    // ...and three DIFFERENT sources are three different addresses, so the check above is not
    // satisfied by everything returning one shared empty literal.
    CHECK(routedPanelId(RouteSource::EntitySelection) != routedPanelId(RouteSource::MaterialAsset));

    // SABOTAGE-FORCED. The four address comparisons above are BLIND to a
    // `return std::string("Inspector").c_str();` implementation, measured: the temporary is SSO, so it
    // lives in routedPanelId's own frame, and two calls from this same caller put that frame at the
    // same address -- the pointers compare EQUAL while both dangle. ASan does not help either, because
    // detect_stack_use_after_return is OFF by default on this lane. STATIC LIFETIME means the bytes
    // survive arbitrary intervening work, so that is what is asserted: hold the pointer, spend the
    // stack, then read it. (The seed reddens RT1 and RT13 too -- this is the case that should SAY so.)
    const char* const held = routedPanelId(RouteSource::EntitySelection);
    for (int i = 0; i < 32; ++i) {
        (void)routeOutcome(RouteSource::MaterialAsset, RouteGuards{.enabled = (i % 2) == 0});
        (void)routedPanelId(RouteSource::ImportableAsset);
    }
    CHECK(std::string_view(held) == INSPECTOR_ID);
}

TEST_CASE("editor: a None source is ALWAYS Drop, whatever the guards say (RT3)") {
    CHECK((routeOutcome(RouteSource::None, RouteGuards{}) == RouteOutcome::Drop));
    // ...including with every transient guard live, which would be a Hold for any real source.
    const RouteGuards allTransient{
        .textInputActive = true, .dragPayloadLive = true, .gizmoDragActive = true, .popupOpen = true};
    CHECK((routeOutcome(RouteSource::None, allTransient) == RouteOutcome::Drop));
    // ...and with every guard at its most permissive, which would be an Apply for any real source.
    CHECK((routeOutcome(RouteSource::None, RouteGuards{}) != RouteOutcome::Apply));
}

TEST_CASE("editor: the preference being off DROPS, for every source (RT4)") {
    const RouteGuards off{.enabled = false};
    CHECK((routeOutcome(RouteSource::EntitySelection, off) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, off) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, off) == RouteOutcome::Drop));
    // ANTI-VACUITY: the same three sources Apply with the default guards, so `off` is what did it.
    CHECK((routeOutcome(RouteSource::EntitySelection, RouteGuards{}) == RouteOutcome::Apply));
}

TEST_CASE("editor: an invalidated source DROPS (RT5)") {
    const RouteGuards gone{.sourceStillValid = false};
    CHECK((routeOutcome(RouteSource::EntitySelection, gone) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, gone) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, gone) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{}) == RouteOutcome::Apply));
}

TEST_CASE("editor: an unavailable target DROPS -- never HOLDS, because it may stay hidden (RT6)") {
    const RouteGuards hidden{.targetAvailable = false};
    CHECK((routeOutcome(RouteSource::EntitySelection, hidden) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, hidden) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, hidden) == RouteOutcome::Drop));
    // The DISTINCTION this case exists for: it is a Drop, and it is NOT a Hold.
    CHECK((routeOutcome(RouteSource::MaterialAsset, hidden) != RouteOutcome::Hold));
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{}) == RouteOutcome::Apply));
}

TEST_CASE("editor: an explicit focus this frame DROPS the route -- a command outranks an inference (RT7)") {
    const RouteGuards explicitWon{.explicitFocusThisFrame = true};
    CHECK((routeOutcome(RouteSource::EntitySelection, explicitWon) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, explicitWon) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, explicitWon) == RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, explicitWon) != RouteOutcome::Hold));
    CHECK((routeOutcome(RouteSource::ImportableAsset, RouteGuards{}) == RouteOutcome::Apply));
}

TEST_CASE("editor: each transient guard ALONE is a HOLD, never a Drop (RT8)") {
    SUBCASE("a text field has the keyboard") {
        const RouteGuards g{.textInputActive = true};
        CHECK((routeOutcome(RouteSource::EntitySelection, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::MaterialAsset, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::ImportableAsset, g) == RouteOutcome::Hold));
    }
    SUBCASE("a drag payload is live") {
        const RouteGuards g{.dragPayloadLive = true};
        CHECK((routeOutcome(RouteSource::EntitySelection, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::MaterialAsset, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::ImportableAsset, g) == RouteOutcome::Hold));
    }
    SUBCASE("an ImGuizmo drag is in flight") {
        const RouteGuards g{.gizmoDragActive = true};
        CHECK((routeOutcome(RouteSource::EntitySelection, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::MaterialAsset, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::ImportableAsset, g) == RouteOutcome::Hold));
    }
    SUBCASE("a popup is open") {
        const RouteGuards g{.popupOpen = true};
        CHECK((routeOutcome(RouteSource::EntitySelection, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::MaterialAsset, g) == RouteOutcome::Hold));
        CHECK((routeOutcome(RouteSource::ImportableAsset, g) == RouteOutcome::Hold));
    }
    SUBCASE("all four at once is still exactly one Hold") {
        const RouteGuards g{
            .textInputActive = true, .dragPayloadLive = true, .gizmoDragActive = true, .popupOpen = true};
        CHECK((routeOutcome(RouteSource::EntitySelection, g) == RouteOutcome::Hold));
    }
}

TEST_CASE("editor: EVERY Drop is tested BEFORE every Hold -- the ordering IS the contract (RT9)") {
    // THE case that catches a reordered routeOutcome. Each pair below is one terminal condition and
    // one transient condition together: if a Hold were tested first, each would read Hold, and a
    // route that can never become valid would be held forever.
    CHECK((routeOutcome(RouteSource::EntitySelection, RouteGuards{.enabled = false, .dragPayloadLive = true}) ==
           RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{.targetAvailable = false, .textInputActive = true}) ==
           RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::ImportableAsset, RouteGuards{.sourceStillValid = false, .popupOpen = true}) ==
           RouteOutcome::Drop));
    CHECK((routeOutcome(RouteSource::EntitySelection,
                        RouteGuards{.explicitFocusThisFrame = true, .gizmoDragActive = true}) == RouteOutcome::Drop));
    // ...and the maximal case: every terminal AND every transient guard live at once.
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{.enabled = false,
                                                                .sourceStillValid = false,
                                                                .targetAvailable = false,
                                                                .explicitFocusThisFrame = true,
                                                                .textInputActive = true,
                                                                .dragPayloadLive = true,
                                                                .gizmoDragActive = true,
                                                                .popupOpen = true}) == RouteOutcome::Drop));
    // ANTI-VACUITY, and the half that makes the four pairs above statements about ORDER rather than
    // about Drop winning everywhere: each transient guard ALONE, on the same source, is a Hold.
    CHECK((routeOutcome(RouteSource::EntitySelection, RouteGuards{.dragPayloadLive = true}) == RouteOutcome::Hold));
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{.textInputActive = true}) == RouteOutcome::Hold));
    CHECK((routeOutcome(RouteSource::ImportableAsset, RouteGuards{.popupOpen = true}) == RouteOutcome::Hold));
    CHECK((routeOutcome(RouteSource::EntitySelection, RouteGuards{.gizmoDragActive = true}) == RouteOutcome::Hold));
}

TEST_CASE("editor: all guards permissive APPLIES, for each of the three real sources (RT10)") {
    CHECK((routeOutcome(RouteSource::EntitySelection, RouteGuards{}) == RouteOutcome::Apply));
    CHECK((routeOutcome(RouteSource::MaterialAsset, RouteGuards{}) == RouteOutcome::Apply));
    CHECK((routeOutcome(RouteSource::ImportableAsset, RouteGuards{}) == RouteOutcome::Apply));
    // The DEFAULTS are the permissive reading -- which is what lets every case above name only the
    // one guard it is about. A default that meant "refuse" would make all of them vacuous.
    const RouteGuards defaults;
    CHECK(defaults.enabled);
    CHECK(defaults.sourceStillValid);
    CHECK(defaults.targetAvailable);
    CHECK_FALSE(defaults.explicitFocusThisFrame);
    CHECK_FALSE(defaults.textInputActive);
    CHECK_FALSE(defaults.dragPayloadLive);
    CHECK_FALSE(defaults.gizmoDragActive);
    CHECK_FALSE(defaults.popupOpen);
}

TEST_CASE("editor: a fresh ContextRouter is enabled, pending nothing, having latched nothing (RT11)") {
    const ContextRouter router;
    CHECK(router.enabled());  // TRUE is the shipping behaviour and therefore the default
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);
}

TEST_CASE("editor: the entity baseline starts where Selection::revision starts, so tick 1 is silent (RT12)") {
    ContextRouter router;
    router.observeEntitySelection(0, true);  // revision 0 == the baseline: NOT a change
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);
    // ANTI-VACUITY: the very next revision DOES latch, so the silence above is about the baseline.
    router.observeEntitySelection(1, true);
    CHECK((router.pending() == RouteSource::EntitySelection));
}

TEST_CASE("editor: a revision bump with a non-empty selection latches EntitySelection (RT13)") {
    ContextRouter router;
    router.observeEntitySelection(1, true);
    CHECK((router.pending() == RouteSource::EntitySelection));
    CHECK(router.latchCount() == 1U);
    CHECK(std::string_view(routedPanelId(router.pending())) == INSPECTOR_ID);
}

TEST_CASE("editor: a bump with an EMPTY selection advances the baseline and latches nothing (RT14)") {
    ContextRouter router;
    router.observeEntitySelection(1, false);  // e.g. clear(), or a pick on empty space
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);
    // The BASELINE moved, which is the half a `nonEmpty`-less implementation gets wrong in the other
    // direction: re-observing revision 1 must still be silent...
    router.observeEntitySelection(1, true);
    CHECK((router.pending() == RouteSource::None));
    // ...and the NEXT revision must latch.
    router.observeEntitySelection(2, true);
    CHECK((router.pending() == RouteSource::EntitySelection));
    CHECK(router.latchCount() == 1U);
}

TEST_CASE("editor: two observations of the SAME source in one tick latch ONCE (RT15)") {
    ContextRouter router;
    router.observeEntitySelection(2, true);
    CHECK(router.latchCount() == 1U);
    router.observeEntitySelection(2, true);  // the same revision: not a change at all
    CHECK(router.latchCount() == 1U);
    // ...and the harder arm: a DIFFERENT revision, same source, with the latch still pending.
    router.observeEntitySelection(3, true);
    CHECK((router.pending() == RouteSource::EntitySelection));
    CHECK(router.latchCount() == 1U);  // latch()'s <= -- a re-latch of the same source is not a latch
}

TEST_CASE("editor: the material arm latches on a change to a non-empty target (RT16)") {
    ContextRouter router;
    router.observeMaterialTarget("");  // the untargeted baseline: not a change
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);

    router.observeMaterialTarget("materials/brick.aeromat");
    CHECK((router.pending() == RouteSource::MaterialAsset));
    CHECK(router.latchCount() == 1U);
    CHECK(std::string_view(routedPanelId(router.pending())) == MATERIAL_ID);

    router.observeMaterialTarget("materials/brick.aeromat");  // the SAME value, next tick
    CHECK(router.latchCount() == 1U);
}

TEST_CASE("editor: a material target clearing to \"\" advances the baseline and latches nothing (RT17)") {
    ContextRouter router;
    router.observeMaterialTarget("materials/brick.aeromat");
    router.clearPending();
    const std::size_t afterFirst = router.latchCount();
    REQUIRE(afterFirst == 1U);

    router.observeMaterialTarget("");  // the session cleared -- a vanished record (AC-14)
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == afterFirst);
    // ...and the baseline really moved: the SAME path again is a change again, and latches again.
    router.observeMaterialTarget("materials/brick.aeromat");
    CHECK((router.pending() == RouteSource::MaterialAsset));
    CHECK(router.latchCount() == afterFirst + 1U);
}

TEST_CASE("editor: an UNSETTLED import observation does nothing AT ALL -- the baseline stays (RT18)") {
    // THE D4 GATE, and the case seed S5 reddens alone. setTarget() sets Idle synchronously on a path
    // change while service() classifies in the post-draw slot, so the honest answer is one tick away.
    // An implementation that advanced the baseline on the Idle tick would consume the change and
    // never see it again -- the panel would simply not come forward, with nothing to observe.
    ContextRouter router;
    router.observeImportTarget("models/a.gltf", /*settled=*/false, /*claimed=*/true);
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);

    router.observeImportTarget("models/a.gltf", /*settled=*/true, /*claimed=*/true);
    CHECK((router.pending() == RouteSource::ImportableAsset));
    CHECK(router.latchCount() == 1U);
    CHECK(std::string_view(routedPanelId(router.pending())) == IMPORT_DETAILS_ID);
}

TEST_CASE("editor: a settled-but-UNCLAIMED import advances the baseline and latches nothing (RT19)") {
    ContextRouter router;
    router.observeImportTarget("textures/a.png", /*settled=*/true, /*claimed=*/false);
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == 0U);
    // The baseline MOVED: re-observing the same unclaimed path stays silent...
    router.observeImportTarget("textures/a.png", true, false);
    CHECK(router.latchCount() == 0U);
    // ...and a DIFFERENT, claimed path latches, so the silence is about `claimed`.
    router.observeImportTarget("models/a.gltf", true, true);
    CHECK((router.pending() == RouteSource::ImportableAsset));
    CHECK(router.latchCount() == 1U);
}

TEST_CASE("editor: a settled, claimed, non-empty import target latches ImportableAsset (RT20)") {
    ContextRouter router;
    router.observeImportTarget("models/a.gltf", true, true);
    CHECK((router.pending() == RouteSource::ImportableAsset));
    CHECK(router.latchCount() == 1U);

    SUBCASE("an EMPTY target never latches, settled and claimed or not") {
        ContextRouter empty;
        empty.observeImportTarget("", true, true);
        CHECK((empty.pending() == RouteSource::None));
        CHECK(empty.latchCount() == 0U);
    }
}

TEST_CASE("editor: unsettled-then-settled on ONE path latches exactly once (RT21)") {
    // The real tick sequence: N+1 Idle, N+2 Imported, N+3.. the same target and the same state. A
    // .blend adds Converting ticks in the middle, and every one of them is the SAME path.
    ContextRouter router;
    router.observeImportTarget("models/a.blend", false, true);  // setTarget's Idle frame
    router.observeImportTarget("models/a.blend", true, true);   // Converting -- settled AND claimed
    router.observeImportTarget("models/a.blend", true, true);   // still Converting
    router.observeImportTarget("models/a.blend", true, true);   // Imported
    CHECK(router.latchCount() == 1U);
    CHECK((router.pending() == RouteSource::ImportableAsset));
}

TEST_CASE("editor: three sources in one tick resolve to EntitySelection, in EITHER order (RT22)") {
    // D17: priority is a property of the enum, not of the call order. RT22 and RT23 are the pair that
    // makes a reordering of EditorApp's three observe* calls a non-event.
    SUBCASE("priority order") {
        ContextRouter router;
        router.observeImportTarget("models/a.gltf", true, true);
        router.observeMaterialTarget("materials/brick.aeromat");
        router.observeEntitySelection(1, true);
        CHECK((router.pending() == RouteSource::EntitySelection));
    }
    SUBCASE("the REVERSE order gives the same winner") {
        ContextRouter router;
        router.observeEntitySelection(1, true);
        router.observeMaterialTarget("materials/brick.aeromat");
        router.observeImportTarget("models/a.gltf", true, true);
        CHECK((router.pending() == RouteSource::EntitySelection));
    }
    SUBCASE("and so does an interleaved one") {
        ContextRouter router;
        router.observeMaterialTarget("materials/brick.aeromat");
        router.observeEntitySelection(1, true);
        router.observeImportTarget("models/a.gltf", true, true);
        CHECK((router.pending() == RouteSource::EntitySelection));
    }
}

TEST_CASE("editor: material beats importable in one tick, in either order (RT23)") {
    SUBCASE("import first") {
        ContextRouter router;
        router.observeImportTarget("models/a.gltf", true, true);
        CHECK((router.pending() == RouteSource::ImportableAsset));
        router.observeMaterialTarget("materials/brick.aeromat");
        CHECK((router.pending() == RouteSource::MaterialAsset));
    }
    SUBCASE("material first -- the import must NOT overwrite it") {
        ContextRouter router;
        router.observeMaterialTarget("materials/brick.aeromat");
        CHECK((router.pending() == RouteSource::MaterialAsset));
        router.observeImportTarget("models/a.gltf", true, true);
        CHECK((router.pending() == RouteSource::MaterialAsset));  // latch()'s <= refuses the demotion
    }
}

TEST_CASE("editor: OFF latches nothing, and re-enabling resurrects NOTHING the user did while off (RT24)") {
    // THE CASE THE CODE-REVIEW ROUND REWROTE, because the version before it pinned a real defect while
    // calling it correct. Gating the observe* EARLY RETURN on `enabled` leaves a STALE baseline behind,
    // so the first observation after the preference comes back on compares against a value from before
    // it went off, sees a "change", and raises a panel for an act minutes old -- a focus steal
    // triggered by ticking a menu item. The contract is: while off, an observation is SEEN and
    // FORGOTTEN. The BASELINE advances; only the LATCH is gated.
    ContextRouter router;
    router.observeEntitySelection(1, true);
    REQUIRE((router.pending() == RouteSource::EntitySelection));

    router.setEnabled(false);
    CHECK_FALSE(router.enabled());
    CHECK((router.pending() == RouteSource::None));  // OFF DROPS a pending route immediately
    const std::size_t latchesWhenDisabled = router.latchCount();

    // Three real acts, performed while the preference is off. None may latch...
    router.observeEntitySelection(2, true);
    router.observeMaterialTarget("materials/brick.aeromat");
    router.observeImportTarget("models/a.gltf", true, true);
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == latchesWhenDisabled);

    router.setEnabled(true);
    CHECK(router.enabled());
    CHECK((router.pending() == RouteSource::None));  // nothing is resurrected by the switch itself
    CHECK(router.latchCount() == latchesWhenDisabled);

    // ...AND THE HALF THE OLD CASE HAD BACKWARDS. Re-observing the SAME three values -- which is
    // exactly what the reconcile block does on the very next tick, because nothing else changed -- is
    // not a change against a baseline that ADVANCED while off, so it latches NOTHING. This is the
    // assertion that goes red if the gate moves back into the early return.
    router.observeEntitySelection(2, true);
    router.observeMaterialTarget("materials/brick.aeromat");
    router.observeImportTarget("models/a.gltf", true, true);
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.latchCount() == latchesWhenDisabled);

    SUBCASE("ANTI-VACUITY: a genuinely FRESH act after re-enabling does latch, on every arm") {
        router.observeEntitySelection(3, true);  // revision 3 -- 2 was performed while off
        CHECK((router.pending() == RouteSource::EntitySelection));
        CHECK(router.latchCount() == latchesWhenDisabled + 1U);
        router.clearPending();

        router.observeMaterialTarget("materials/stone.aeromat");  // a DIFFERENT material
        CHECK((router.pending() == RouteSource::MaterialAsset));
        CHECK(router.latchCount() == latchesWhenDisabled + 2U);
        router.clearPending();

        router.observeImportTarget("models/b.gltf", true, true);  // a DIFFERENT model
        CHECK((router.pending() == RouteSource::ImportableAsset));
        CHECK(router.latchCount() == latchesWhenDisabled + 3U);
    }

    SUBCASE("the import arm's UNSETTLED rule is independent of the preference and survives it") {
        // D4 is about the SESSION's state machine, so `!settled` stays in the early return while
        // `enabled` moved out of it. An unsettled observation made while OFF must still leave the
        // baseline alone, or the settled tick that follows has nothing left to see.
        ContextRouter fresh;
        fresh.setEnabled(false);
        fresh.observeImportTarget("models/c.gltf", /*settled=*/false, /*claimed=*/true);
        fresh.setEnabled(true);
        fresh.observeImportTarget("models/c.gltf", /*settled=*/true, /*claimed=*/true);
        CHECK((fresh.pending() == RouteSource::ImportableAsset));
        CHECK(fresh.latchCount() == 1U);
    }
}

TEST_CASE("editor: clearPending returns to None and leaves every baseline intact (RT25)") {
    ContextRouter router;
    router.observeMaterialTarget("materials/brick.aeromat");
    REQUIRE((router.pending() == RouteSource::MaterialAsset));

    router.clearPending();
    CHECK((router.pending() == RouteSource::None));
    CHECK(router.enabled());  // clearing a latch is not turning the feature off

    // The BASELINE survived: the same value is not a change, so nothing re-latches next tick. This is
    // what keeps a raised panel from being raised again on every subsequent frame.
    router.observeMaterialTarget("materials/brick.aeromat");
    CHECK((router.pending() == RouteSource::None));
    // ...and a real change still latches.
    router.observeMaterialTarget("materials/stone.aeromat");
    CHECK((router.pending() == RouteSource::MaterialAsset));
}

TEST_CASE("editor: latchCount is a LIFETIME counter across clearPending (RT26)") {
    ContextRouter router;
    router.observeMaterialTarget("a.aeromat");
    router.clearPending();
    router.observeMaterialTarget("b.aeromat");
    router.clearPending();
    router.observeEntitySelection(1, true);
    CHECK(router.latchCount() == 3U);
    CHECK((router.pending() == RouteSource::EntitySelection));
    router.clearPending();
    CHECK(router.latchCount() == 3U);  // clearing the slot never rewinds the count
}

TEST_CASE("editor: the routing types' shape, priority values and noexcept contract (RT27)") {
    // THE VALUE PIN. RouteSource's numeric order IS its priority, so an enumerator reordered for
    // "readability" is a behaviour change, and this is where it stops.
    static_assert(std::is_same_v<std::underlying_type_t<RouteSource>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<RouteOutcome>, std::uint8_t>);
    static_assert(static_cast<std::uint8_t>(RouteSource::None) == 0U);
    static_assert(static_cast<std::uint8_t>(RouteSource::ImportableAsset) == 1U);
    static_assert(static_cast<std::uint8_t>(RouteSource::MaterialAsset) == 2U);
    static_assert(static_cast<std::uint8_t>(RouteSource::EntitySelection) == 3U);
    static_assert(static_cast<std::uint8_t>(RouteOutcome::Drop) == 0U);
    static_assert(static_cast<std::uint8_t>(RouteOutcome::Hold) == 1U);
    static_assert(static_cast<std::uint8_t>(RouteOutcome::Apply) == 2U);
    // The ORDER stated as the relation the router relies on, not just as four constants.
    static_assert(static_cast<std::uint8_t>(RouteSource::None) <
                  static_cast<std::uint8_t>(RouteSource::ImportableAsset));
    static_assert(static_cast<std::uint8_t>(RouteSource::ImportableAsset) <
                  static_cast<std::uint8_t>(RouteSource::MaterialAsset));
    static_assert(static_cast<std::uint8_t>(RouteSource::MaterialAsset) <
                  static_cast<std::uint8_t>(RouteSource::EntitySelection));

    static_assert(noexcept(routeOutcome(RouteSource::None, RouteGuards{})));
    static_assert(std::is_nothrow_move_constructible_v<ContextRouter>);
    static_assert(std::is_nothrow_move_assignable_v<ContextRouter>);
    static_assert(std::is_default_constructible_v<ContextRouter>);
    const ContextRouter router;
    static_assert(noexcept(router.enabled()));
    static_assert(noexcept(router.pending()));
    static_assert(noexcept(router.latchCount()));

    // A MOVED router carries its baselines and its count with it -- EditorApp is movable and
    // create() returns an optional, so this is the shape the app's own move relies on.
    ContextRouter source;
    source.observeMaterialTarget("a.aeromat");
    const ContextRouter moved = std::move(source);
    CHECK(moved.latchCount() == 1U);
    CHECK((moved.pending() == RouteSource::MaterialAsset));
}

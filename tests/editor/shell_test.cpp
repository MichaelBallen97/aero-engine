// aero_editor_shell_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here (the aero_reflect_meta_test / aero_editor_imgui_test
// precedent). Tier-0 and UNGATED (task 2.1.3, D10, AC-10): no GPU, no window, no ImGui context --
// it must pass identically with AERO_REQUIRE_GPU unset and set. It links aero::editor_core (the
// registry + pacing policy live there) but never constructs a platform::Context/Window/rhi::Device/
// EditorApp/ImGuiLayer and never names an ImGui symbol.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/editor/editor_app.hpp>
#include <aero/editor/panel.hpp>
#include <aero/editor/panel_registry.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

int nullIdDtorCount = 0;
int emptyIdDtorCount = 0;

class TestPanel final : public engine::editor::Panel {
public:
    explicit TestPanel(const char* id, engine::editor::DockSlot slot = engine::editor::DockSlot::Center) noexcept
        : panelId(id), dockSlot(slot) {}
    [[nodiscard]] const char* id() const noexcept override { return panelId; }
    [[nodiscard]] engine::editor::DockSlot defaultDockSlot() const noexcept override { return dockSlot; }
    void onDraw() override { ++drawCount; }
    int drawCount = 0;  // public: misc-non-private-member-variables-in-classes is OFF

private:
    const char* panelId;
    engine::editor::DockSlot dockSlot;
};

// A heap-tracking subclass: its dtor increments a file-static counter, which is what proves a
// rejected panel is actually DESTROYED (not leaked) by PanelRegistry::add() (AC-4(c)).
class NullIdPanel final : public engine::editor::Panel {
public:
    ~NullIdPanel() override { ++nullIdDtorCount; }
    [[nodiscard]] const char* id() const noexcept override { return nullptr; }
    void onDraw() override {}
};

class EmptyIdPanel final : public engine::editor::Panel {
public:
    ~EmptyIdPanel() override { ++emptyIdDtorCount; }
    [[nodiscard]] const char* id() const noexcept override { return ""; }
    void onDraw() override {}
};

class TypedPanel final : public engine::editor::Panel {
public:
    [[nodiscard]] const char* id() const noexcept override { return "Typed"; }
    void onDraw() override {}
    int marker = 7;
};

// Overrides ONLY id() and onDraw() -- the minimal legal Panel, used to pin the Panel base's own
// defaults (title() == id(), defaultDockSlot() == Center, every PanelOptions flag false).
class MinimalPanel final : public engine::editor::Panel {
public:
    [[nodiscard]] const char* id() const noexcept override { return "Minimal"; }
    void onDraw() override {}
};

}  // namespace

TEST_CASE("editor: registry starts empty") {
    engine::editor::PanelRegistry registry;
    CHECK(registry.count() == 0);
    CHECK(registry.find("x") == nullptr);
    CHECK_FALSE(registry.visible("x"));
}

TEST_CASE("editor: add returns a non-null stable pointer") {
    engine::editor::PanelRegistry registry;
    engine::editor::Panel* panel = registry.add(std::make_unique<TestPanel>("A"));
    REQUIRE(panel != nullptr);
    CHECK(registry.count() == 1);
    CHECK(&registry.panelAt(0) == panel);
}

TEST_CASE("editor: panel addresses are stable across later adds") {
    engine::editor::PanelRegistry registry;
    engine::editor::Panel* first = registry.add(std::make_unique<TestPanel>("First"));
    engine::editor::Panel* second = registry.add(std::make_unique<TestPanel>("Second"));
    engine::editor::Panel* third = registry.add(std::make_unique<TestPanel>("Third"));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(third != nullptr);

    // 20 more registrations force the underlying vector to reallocate at least once; the panel
    // OBJECTS must not move even though the vector's storage does (E14 -- the unique_ptr indirection).
    std::vector<std::string> extraIds;
    extraIds.reserve(20);
    for (int i = 0; i < 20; ++i) {
        extraIds.push_back("Extra" + std::to_string(i));
    }
    for (const std::string& id : extraIds) {
        REQUIRE(registry.add(std::make_unique<TestPanel>(id.c_str())) != nullptr);
    }

    CHECK(first == &registry.panelAt(0));
    CHECK(second == &registry.panelAt(1));
    CHECK(third == &registry.panelAt(2));
}

TEST_CASE("editor: duplicate ids are rejected") {
    engine::editor::PanelRegistry registry;
    engine::editor::Panel* first = registry.add(std::make_unique<TestPanel>("A"));
    REQUIRE(first != nullptr);

    engine::editor::Panel* second = registry.add(std::make_unique<TestPanel>("A"));
    CHECK(second == nullptr);
    CHECK(registry.count() == 1);
    CHECK(registry.find("A") == first);
}

TEST_CASE("editor: null panel, null id and empty id are rejected") {
    engine::editor::PanelRegistry registry;

    CHECK(registry.add(nullptr) == nullptr);
    CHECK(registry.count() == 0);

    const int nullDtorsBefore = nullIdDtorCount;
    CHECK(registry.add(std::make_unique<NullIdPanel>()) == nullptr);
    CHECK(registry.count() == 0);
    CHECK(nullIdDtorCount == nullDtorsBefore + 1);

    const int emptyDtorsBefore = emptyIdDtorCount;
    CHECK(registry.add(std::make_unique<EmptyIdPanel>()) == nullptr);
    CHECK(registry.count() == 0);
    CHECK(emptyIdDtorCount == emptyDtorsBefore + 1);
}

TEST_CASE("editor: emplace returns a typed pointer") {
    engine::editor::PanelRegistry registry;
    auto* typed = registry.emplace<TypedPanel>();
    REQUIRE(typed != nullptr);
    CHECK(typed->marker == 7);

    auto* rejected = registry.emplace<NullIdPanel>();
    CHECK(rejected == nullptr);
}

TEST_CASE("editor: visibility defaults to true and round-trips by index") {
    engine::editor::PanelRegistry registry;
    registry.add(std::make_unique<TestPanel>("A"));

    CHECK(registry.visibleAt(0));
    registry.setVisibleAt(0, false);
    CHECK_FALSE(registry.visibleAt(0));
    registry.setVisibleAt(0, true);
    CHECK(registry.visibleAt(0));
}

TEST_CASE("editor: visibility by id agrees with visibility by index") {
    engine::editor::PanelRegistry registry;
    registry.add(std::make_unique<TestPanel>("A"));
    registry.add(std::make_unique<TestPanel>("B"));

    registry.setVisible("B", false);
    CHECK_FALSE(registry.visibleAt(1));
    CHECK_FALSE(registry.visible("B"));
    registry.toggle("B");
    CHECK(registry.visibleAt(1));
    CHECK(registry.visible("B"));

    // the reverse direction: by-index writes agree with the by-id reader.
    registry.setVisibleAt(0, false);
    CHECK_FALSE(registry.visible("A"));
    registry.toggle("A");
    CHECK(registry.visible("A"));
}

TEST_CASE("editor: unknown ids are logged no-ops") {
    engine::editor::PanelRegistry registry;
    registry.add(std::make_unique<TestPanel>("A"));

    registry.setVisible("nope", false);
    CHECK(registry.visibleAt(0));
    CHECK(registry.count() == 1);

    registry.toggle("nope");
    CHECK(registry.visibleAt(0));
    CHECK(registry.count() == 1);

    CHECK(registry.find("nope") == nullptr);
    CHECK_FALSE(registry.visible("nope"));

    // the C6 null-id guard: a null id must never match and must never format() a null const char*.
    registry.setVisible(nullptr, false);
    CHECK(registry.visibleAt(0));
    registry.toggle(nullptr);
    CHECK(registry.visibleAt(0));
    CHECK(registry.find(nullptr) == nullptr);
    CHECK_FALSE(registry.visible(nullptr));
}

TEST_CASE("editor: iteration order is registration order") {
    engine::editor::PanelRegistry registry;
    registry.add(std::make_unique<TestPanel>("A"));
    registry.add(std::make_unique<TestPanel>("B"));
    registry.add(std::make_unique<TestPanel>("C"));
    registry.add(std::make_unique<TestPanel>("D"));
    registry.add(std::make_unique<TestPanel>("E"));

    const std::vector<std::string> expected = {"A", "B", "C", "D", "E"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(std::string(registry.panelAt(i).id()) == expected[i]);
    }
}

TEST_CASE("editor: Panel defaults") {
    const MinimalPanel panel;
    CHECK(panel.title() == panel.id());  // pointer-equal
    CHECK(panel.defaultDockSlot() == engine::editor::DockSlot::Center);
    const engine::editor::PanelOptions opts = panel.options();
    CHECK_FALSE(opts.noScrollbar);
    CHECK_FALSE(opts.noPadding);
    CHECK_FALSE(opts.hasMenuBar);
}

TEST_CASE("editor: const overloads compile and agree") {
    engine::editor::PanelRegistry registry;
    registry.add(std::make_unique<TestPanel>("A"));

    const engine::editor::PanelRegistry& constRegistry = registry;
    CHECK(&constRegistry.panelAt(0) == &registry.panelAt(0));
    CHECK(constRegistry.find("A") == registry.find("A"));
    CHECK(constRegistry.visible("A") == registry.visible("A"));
}

TEST_CASE("editor: registry move leaves the source empty and preserves addresses") {
    static_assert(std::is_nothrow_move_constructible_v<engine::editor::PanelRegistry>);
    static_assert(std::is_nothrow_move_assignable_v<engine::editor::PanelRegistry>);

    // Wrapped in std::optional (the transform_test.cpp/rhi_device_test.cpp precedent): moving *source
    // rather than a bare local, and reading the moved-from state back through source-> afterward, is
    // what keeps bugprone-use-after-move from flagging a deliberate moved-from-state assertion.
    std::optional<engine::editor::PanelRegistry> source;
    source.emplace();
    engine::editor::Panel* a = source->add(std::make_unique<TestPanel>("A"));
    engine::editor::Panel* b = source->add(std::make_unique<TestPanel>("B"));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    engine::editor::PanelRegistry moveConstructed = std::move(*source);
    CHECK(moveConstructed.count() == 2);
    CHECK(&moveConstructed.panelAt(0) == a);
    CHECK(&moveConstructed.panelAt(1) == b);
    CHECK(source->count() == 0);

    // Re-wrap moveConstructed as the source of the SECOND move, for the same reason as above.
    std::optional<engine::editor::PanelRegistry> moveConstructedSrc(std::move(moveConstructed));
    engine::editor::PanelRegistry moveAssigned;
    moveAssigned.add(std::make_unique<TestPanel>("C"));
    moveAssigned = std::move(*moveConstructedSrc);
    CHECK(moveAssigned.count() == 2);
    CHECK(&moveAssigned.panelAt(0) == a);
    CHECK(&moveAssigned.panelAt(1) == b);
    CHECK(moveConstructedSrc->count() == 0);
}

TEST_CASE("editor: framePaceSleepMs covers all five branches") {
    using engine::editor::framePaceSleepMs;
    using engine::editor::MINIMIZED_SLEEP_MS;

    static_assert(MINIMIZED_SLEEP_MS == 4U);
    static_assert(noexcept(framePaceSleepMs(true, true, 1.0F, 1.0F)));

    SUBCASE("minimized, unfocused, capped -> MINIMIZED_SLEEP_MS") {
        CHECK(framePaceSleepMs(false, false, 20.0F, 0.0F) == 4U);
    }
    SUBCASE("minimized beats focused (E12)") { CHECK(framePaceSleepMs(false, true, 20.0F, 0.0F) == 4U); }
    SUBCASE("minimized beats a disabled cap") { CHECK(framePaceSleepMs(false, false, 0.0F, 0.0F) == 4U); }
    SUBCASE("focused: vsync paces, no sleep") { CHECK(framePaceSleepMs(true, true, 20.0F, 0.0F) == 0U); }
    SUBCASE("unfocused, cap disabled (zero)") { CHECK(framePaceSleepMs(true, false, 0.0F, 0.0F) == 0U); }
    SUBCASE("unfocused, cap disabled (negative)") { CHECK(framePaceSleepMs(true, false, -5.0F, 0.0F) == 0U); }
    SUBCASE("unfocused, cap disabled (NaN, E18)") {
        CHECK(framePaceSleepMs(true, false, std::numeric_limits<float>::quiet_NaN(), 0.0F) == 0U);
    }
    SUBCASE("unfocused, full budget") { CHECK(framePaceSleepMs(true, false, 20.0F, 0.0F) == 50U); }
    SUBCASE("unfocused, partial budget remaining") { CHECK(framePaceSleepMs(true, false, 20.0F, 30.0F) == 20U); }
    SUBCASE("unfocused, elapsed equals budget") { CHECK(framePaceSleepMs(true, false, 20.0F, 50.0F) == 0U); }
    SUBCASE("unfocused, elapsed exceeds budget") { CHECK(framePaceSleepMs(true, false, 20.0F, 80.0F) == 0U); }
    SUBCASE("unfocused, huge cap (E18, no division blow-up)") {
        CHECK(framePaceSleepMs(true, false, 1.0e6F, 0.0F) == 0U);
    }
}

TEST_CASE("editor: DockSlot layout contract") {
    using engine::editor::DockSlot;
    static_assert(sizeof(DockSlot) == 1);
    static_assert(std::is_same_v<std::underlying_type_t<DockSlot>, std::uint8_t>);
    static_assert(static_cast<std::uint8_t>(DockSlot::Center) == 0);
    static_assert(static_cast<std::uint8_t>(DockSlot::Left) == 1);
    static_assert(static_cast<std::uint8_t>(DockSlot::Right) == 2);
    static_assert(static_cast<std::uint8_t>(DockSlot::Bottom) == 3);
}

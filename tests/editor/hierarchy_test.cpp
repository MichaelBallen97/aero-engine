// tests/editor/hierarchy_test.cpp — task 2.2.1: the editor's scene-state layer, tier-0 and UNGATED.
// The second TU of aero_editor_shell_test (which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here). No GPU, no window, no ImGui context, no files, no
// randomness: it must pass identically with AERO_REQUIRE_GPU unset and set (AC-19).
//
// It covers Selection (selection.hpp), the pure structural ops + RootOrder + seedDefaultScene
// (entity_ops.hpp) and PanelContext (panel_context.hpp). The HierarchyPanel itself is src-private and
// ImGui-bound: its Begin/End + TreePop balance is proven by aero_editor_imgui_test on a real GPU,
// which is the only place that assertion means anything (I3).
#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- the E12/AC-13 proof
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::MeshRenderer;
using engine::Transform;
using engine::World;
using engine::editor::Selection;
using engine::scene::internal::registerComponent;

namespace {

// Every helper here is a free function over a local World -- no fixture class, matching
// scene_test.cpp / transform_test.cpp.
[[nodiscard]] std::vector<Entity> childrenOf(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&out](Entity c) { out.push_back(c); });
    return out;
}

[[nodiscard]] std::vector<Entity> rootsOf(const World& world) {
    std::vector<Entity> out;
    world.eachEntity([&](Entity e) {
        if (!world.parent(e).valid()) {
            out.push_back(e);
        }
    });
    return out;
}

}  // namespace

// ---- Selection ---------------------------------------------------------------------------------

TEST_CASE("editor: Selection starts empty and rejects the null entity (I5)") {
    Selection s;
    CHECK(s.empty());
    CHECK(s.count() == 0);
    CHECK(s.primary() == Entity{});
    CHECK(s.entities().empty());
    CHECK_FALSE(s.contains(Entity{}));

    s.add(Entity{});
    CHECK(s.empty());
    s.set(Entity{});
    CHECK(s.empty());
    s.toggle(Entity{});
    CHECK(s.empty());
    CHECK_FALSE(s.contains(Entity{}));
}

TEST_CASE("editor: Selection set/add/remove/toggle and the primary (D2)") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    Selection s;

    s.set(a);
    CHECK(s.count() == 1);
    CHECK(s.primary() == a);

    s.add(b);
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);  // most recently ADDED
    s.add(b);                 // no-op, and does NOT re-primary
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);

    s.add(c);
    CHECK(s.primary() == c);
    s.remove(c);  // removing the primary re-primaries to the last survivor
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);
    s.remove(a);  // removing a NON-primary leaves the primary alone
    CHECK(s.primary() == b);
    s.remove(a);  // absent -> no-op
    CHECK(s.count() == 1);

    s.toggle(a);
    CHECK(s.contains(a));
    CHECK(s.primary() == a);
    s.toggle(a);
    CHECK_FALSE(s.contains(a));
    CHECK(s.primary() == b);

    s.set(c);  // set REPLACES
    CHECK(s.count() == 1);
    CHECK(s.contains(c));
    CHECK_FALSE(s.contains(b));

    s.clear();
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection::setAll replaces wholesale, dedupes and skips nulls") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    Selection s;
    s.set(a);

    const std::vector<Entity> batch{b, a, b, Entity{}, a};
    s.setAll(batch);
    CHECK(s.count() == 2);
    CHECK(s.contains(a));
    CHECK(s.contains(b));
    CHECK(s.primary() == a);  // the LAST newly-added survivor
    CHECK(s.entities()[0] == b);  // input order preserved
    CHECK(s.entities()[1] == a);

    s.setAll(std::span<const Entity>{});
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection::prune drops dead handles and fixes the primary (I5)") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    Selection s;
    s.add(a);
    s.add(b);
    s.add(c);
    REQUIRE(s.primary() == c);

    REQUIRE(w.destroy(c));
    CHECK(s.prune(w) == 1);
    CHECK(s.count() == 2);
    CHECK(s.primary() == b);  // the dropped primary follows to the last survivor
    CHECK(s.prune(w) == 0);   // idempotent

    REQUIRE(w.destroy(a));
    REQUIRE(w.destroy(b));
    CHECK(s.prune(w) == 2);
    CHECK(s.empty());
    CHECK(s.primary() == Entity{});
}

TEST_CASE("editor: Selection's shape and noexcept contract") {
    static_assert(std::is_nothrow_move_constructible_v<Selection>);
    static_assert(std::is_nothrow_move_assignable_v<Selection>);
    static_assert(std::is_default_constructible_v<Selection>);
    const Selection s;
    static_assert(noexcept(s.empty()));
    static_assert(noexcept(s.count()));
    static_assert(noexcept(s.contains(Entity{})));
    static_assert(noexcept(s.primary()));
    static_assert(noexcept(s.entities()));
}

// ---- PanelContext (AC-8/D7) ----------------------------------------------------------------------

TEST_CASE("editor: PanelContext binds references, it does not copy (D7)") {
    World w;
    Selection s;
    engine::editor::PanelContext context{w, s};

    CHECK(&context.world == &w);
    CHECK(&context.selection == &s);

    // A write through the context is visible through the originals -- the reference proof, not a
    // tautology: a by-value aggregate would pass the address check only by accident and fail here.
    const Entity e = context.world.create();
    context.selection.add(e);
    CHECK(w.alive(e));
    CHECK(s.contains(e));
    CHECK(s.primary() == e);

    static_assert(std::is_aggregate_v<engine::editor::PanelContext>);
    static_assert(!std::is_copy_assignable_v<engine::editor::PanelContext>);  // reference members
}

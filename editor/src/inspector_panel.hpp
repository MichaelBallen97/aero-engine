#pragma once
// Aero Engine -- the reflection-driven Inspector panel (task 2.2.2). SRC-PRIVATE and the ONLY new
// ImGui TU this task adds: this header itself is ImGui-free (registered by editor_app.cpp, which
// never sees ImGui), but its .cpp is where every ImGui call lives.
//
// FRAME SHAPE -- onDraw is exactly FOUR phases, in this order:
//   1. reconcile  drop any edit-cache entry whose target no longer resolves (E3)
//   2. build      buildInspectorModel(context.world, primary, model) -- D15 scratch, rebuilt fresh
//   3. draw       walk the model, drawing widgets; a VALUE edit writes through the seam IMMEDIATELY
//                 (safe mid-draw -- it touches no storage structure, D9); Add/Remove are recorded
//                 into `pending`, never applied here
//   4. apply      one switch over `pending` -- the ONLY place a component is added or removed
// No walk here is recursive (F22) -- the model is two flat vectors, never a tree.
#include <aero/editor/component_ops.hpp>
#include <aero/editor/inspector_model.hpp>
#include <aero/editor/panel.hpp>
#include <aero/scene/entity.hpp>

#include <cstdint>
#include <string>

namespace engine::editor {

class InspectorPanel final : public Panel {
public:
    // D20: the id is the ImGui window name AND the imgui.ini settings key. It stays "Inspector" --
    // renaming it orphans every existing user's saved layout for this panel (F16).
    [[nodiscard]] const char* id() const noexcept override { return "Inspector"; }
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    void onDraw(PanelContext& context) override;

private:
    enum class ActionKind : std::uint8_t { None = 0, AddComponent, RemoveComponent };
    struct PendingAction {
        ActionKind kind = ActionKind::None;
        ComponentTypeId type{};
    };

    // At most one widget is active at a time, so ONE slot each. Keyed on (entity, type, field) so
    // reconcile can drop a cache whose target no longer resolves (E3).
    struct EditKey {
        Entity entity{};
        ComponentTypeId type{};
        std::string field;
        bool active = false;

        [[nodiscard]] bool matches(Entity e, ComponentTypeId t, std::string_view f) const noexcept {
            return entity == e && type == t && field == f;
        }
    };
    struct QuatEditCache : EditKey {
        Vec3 eulerDegrees;
    };
    struct StringEditCache : EditKey {
        std::string buffer;
    };

    void drawComponent(PanelContext& context, Entity primary, const ComponentEntry& entry);
    void drawField(PanelContext& context, Entity primary, const ComponentEntry& entry, const FieldEntry& field);
    void applyPending(PanelContext& context, Entity primary);

    InspectorModel model;  // D15 scratch, rebuilt every frame
    PendingAction pending{};
    QuatEditCache quatCache;
    StringEditCache stringCache;
    std::string labelScratch;
    std::string shortNameScratch;
};

}  // namespace engine::editor

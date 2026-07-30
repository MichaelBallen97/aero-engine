#pragma once
// Aero Engine -- the one concrete command task 2.4.1 ships (D1): an undoable local-Transform write,
// wrapping 2.3.3's transform_ops seam exactly as that header said it would be wrapped
// (transform_ops.hpp:2-5). PUBLIC and ImGui/entt/ImGuizmo-free like every editor/include header, which
// is what lets the tier-0 test drive it.

#include <aero/editor/command_stack.hpp>
#include <aero/scene/entity.hpp>
#include <aero/scene/transform.hpp>

#include <string_view>

namespace engine::editor {

// The Edit menu shows "Undo Transform" / "Redo Transform". ONE label for all three operations: with
// merging, one drag is one entry, and the entry is unambiguous without naming translate/rotate/scale
// (D18). Exposed as a constant so the test asserts against it, never against a magic literal.
inline constexpr std::string_view TRANSFORM_COMMAND_LABEL = "Transform";

class TransformCommand final : public Command {
public:
    // `before` is the entity's local Transform as it stood BEFORE this edit; `after` is the value to
    // write. Both by const& (Transform is 40 bytes; the writeTransform signature's own spelling).
    TransformCommand(Entity entity, const Transform& before, const Transform& after);

    bool redo(CommandContext& context) override;
    bool undo(CommandContext& context) override;
    [[nodiscard]] std::string_view label() const noexcept override;
    // True iff `incoming` is a TransformCommand on the SAME entity. Keeps our `before`, takes their
    // `after` -- which is what makes a 200-frame drag one undo step whose before is the drag's start.
    bool mergeWith(const Command& incoming) override;

    // Accessors carry the distinct name (the RenderTarget::depthFormatValue <-> depthFormat() rule);
    // the MEMBERS are beforeValue/afterValue.
    [[nodiscard]] Entity entity() const noexcept;
    [[nodiscard]] const Transform& before() const noexcept;
    [[nodiscard]] const Transform& after() const noexcept;

private:
    bool write(World& world, const Transform& value);

    Entity target{};
    Transform beforeValue{};
    Transform afterValue{};
};

}  // namespace engine::editor

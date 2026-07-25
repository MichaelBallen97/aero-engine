#pragma once
// Aero Engine — PanelRegistry (task 2.1.3, D2). Owns every registered Panel, knows their draw/
// View-menu order and visibility. ImGui-FREE BY RULE (D9/AC-3) — this header stays includable from
// a TU that has never seen imgui.h, which is what lets the tier-0 shell test unit-test the registry
// with no ImGui context.

#include <aero/editor/panel.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace engine::editor {

class PanelRegistry {
public:
    PanelRegistry() = default;
    ~PanelRegistry() = default;
    PanelRegistry(PanelRegistry&&) noexcept = default;
    PanelRegistry& operator=(PanelRegistry&&) noexcept = default;
    PanelRegistry(const PanelRegistry&) = delete;
    PanelRegistry& operator=(const PanelRegistry&) = delete;

    // Registers `panel` and returns a NON-OWNING, STABLE pointer to it: the registry owns it through
    // a unique_ptr, so the Panel object never moves even when the index vector reallocates (which is
    // what makes registering a panel from inside another panel's onDraw() safe — E14).
    // Returns nullptr + AERO_LOG_ERROR (and DESTROYS the argument) when: panel is null; id() is null
    // or empty; id() duplicates an already-registered one (ImGui would silently merge two windows
    // sharing a name — E5).
    Panel* add(std::unique_ptr<Panel> panel);

    // Typed convenience: registry.emplace<HierarchyPanel>(world) -> HierarchyPanel* (nullptr if
    // rejected). static_cast of a null Panel* is well-defined and yields nullptr.
    template <class T, class... Args>
    T* emplace(Args&&... args) {
        return static_cast<T*>(add(std::make_unique<T>(std::forward<Args>(args)...)));
    }

    [[nodiscard]] std::size_t count() const noexcept;

    // Index access. Registration order == draw order == View-menu order (D5). Indices are stable
    // (add() only appends). PRECONDITION index < count(), asserted in debug (docs/04).
    [[nodiscard]] Panel& panelAt(std::size_t index) noexcept;
    [[nodiscard]] const Panel& panelAt(std::size_t index) const noexcept;
    [[nodiscard]] bool visibleAt(std::size_t index) const noexcept;
    void setVisibleAt(std::size_t index, bool visible) noexcept;

    // By id. An unknown id is a logged no-op / false / nullptr — never an assert: ids come from
    // panels and from menu code, and a typo must be loud but non-fatal.
    [[nodiscard]] Panel* find(const char* id) noexcept;
    [[nodiscard]] const Panel* find(const char* id) const noexcept;
    [[nodiscard]] bool visible(const char* id) const noexcept;
    void setVisible(const char* id, bool visible);
    void toggle(const char* id);

private:
    [[nodiscard]] std::size_t indexOf(const char* id) const noexcept;  // count() when absent

    struct Entry {
        std::unique_ptr<Panel> panel;
        bool visible = true;
    };
    std::vector<Entry> entries;
};

}  // namespace engine::editor

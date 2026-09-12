#pragma once
// Aero Engine — PanelRegistry (task 2.1.3, D2). Owns every registered Panel, knows their draw/
// View-menu order and visibility. ImGui-FREE BY RULE (D9/AC-3) — this header stays includable from
// a TU that has never seen imgui.h, which is what lets the tier-0 shell test unit-test the registry
// with no ImGui context.

#include <aero/editor/panel.hpp>

#include <cstddef>
#include <cstdint>  // task E.3.2: drawnCountAt/drawnCount return std::uint64_t
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

    // ---- task E.3.2: the drawn-frame counter ------------------------------------------------------
    // HOW MANY FRAMES THIS PANEL'S onDraw() HAS RUN -- i.e. how many frames ImGui::Begin returned true
    // for it. For a DOCKED panel that is exactly "was it the selected tab", which is the property
    // E.3.2 delivers and the only property no tier in this tree could previously read: a tabbed-away
    // panel is skipped entirely by drawPanels (shell_ui.cpp:356), drains no pending action and leaves
    // no other trace. 3.1.3's own log entry records TWO test attempts that passed while executing none
    // of the code they named, for exactly this reason.
    //
    // Written by drawPanels() and by NOTHING else. Monotonic; a LIFETIME counter, never reset -- so
    // "this panel has not drawn since tick N" is a DELTA the caller takes, never a value this class
    // reports. Still ImGui-free: this is a std::uint64_t, and Begin's answer arrives as a bool.
    void noteDrawn(std::size_t index) noexcept;  // PRECONDITION index < count(), asserted in debug
    [[nodiscard]] std::uint64_t drawnCountAt(std::size_t index) const noexcept;  // PRECONDITION as above
    // 0 for an UNKNOWN id and 0 for a registered panel that has never drawn -- the two are
    // deliberately INDISTINGUISHABLE here, because an unknown id is already a logged no-op everywhere
    // else in this class and a second reporting convention would be a second truth. A caller that
    // needs the difference asks find(id) first, and I154 does exactly that.
    [[nodiscard]] std::uint64_t drawnCount(const char* id) const noexcept;

private:
    [[nodiscard]] std::size_t indexOf(const char* id) const noexcept;  // count() when absent

    struct Entry {
        std::unique_ptr<Panel> panel;
        bool visible = true;
        std::uint64_t drawn = 0;  // task E.3.2
    };
    std::vector<Entry> entries;
};

}  // namespace engine::editor

// Aero Engine — PanelRegistry's out-of-line half (task 2.1.3, D2). Pure std C++ + AERO_LOG_* — no
// ImGui, no SDL, no engine subsystem beyond <aero/core/log.hpp>.
#include <aero/core/log.hpp>
#include <aero/editor/panel_registry.hpp>

#include <cassert>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {

// A null id never matches anything — constructing std::string_view from a null const char* is UB,
// so this guards before doing so (C6).
[[nodiscard]] bool idsEqual(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::string_view{a} == std::string_view{b};
}

// AERO_LOG_* formats through std::format, and formatting a null const char* with "{}" is UB (C6).
[[nodiscard]] std::string_view idTextOrNull(const char* id) noexcept {
    return id == nullptr ? std::string_view{"(null)"} : std::string_view{id};
}

}  // namespace

std::size_t PanelRegistry::count() const noexcept { return entries.size(); }

Panel& PanelRegistry::panelAt(std::size_t index) noexcept {
    assert(index < entries.size());
    return *entries[index].panel;
}

const Panel& PanelRegistry::panelAt(std::size_t index) const noexcept {
    assert(index < entries.size());
    return *entries[index].panel;
}

bool PanelRegistry::visibleAt(std::size_t index) const noexcept {
    assert(index < entries.size());
    return entries[index].visible;
}

void PanelRegistry::setVisibleAt(std::size_t index, bool visible) noexcept {
    assert(index < entries.size());
    entries[index].visible = visible;
}

std::size_t PanelRegistry::indexOf(const char* id) const noexcept {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (idsEqual(entries[i].panel->id(), id)) {
            return i;
        }
    }
    return entries.size();
}

Panel* PanelRegistry::find(const char* id) noexcept {
    const std::size_t index = indexOf(id);
    return index < entries.size() ? entries[index].panel.get() : nullptr;
}

const Panel* PanelRegistry::find(const char* id) const noexcept {
    const std::size_t index = indexOf(id);
    return index < entries.size() ? entries[index].panel.get() : nullptr;
}

bool PanelRegistry::visible(const char* id) const noexcept {
    const std::size_t index = indexOf(id);
    return index < entries.size() && entries[index].visible;
}

Panel* PanelRegistry::add(std::unique_ptr<Panel> panel) {
    if (panel == nullptr) {
        AERO_LOG_ERROR("editor: PanelRegistry::add: rejected a null panel");
        return nullptr;
    }
    const char* id = panel->id();
    if (id == nullptr || id[0] == '\0') {
        AERO_LOG_ERROR("editor: PanelRegistry::add: rejected a panel with a null/empty id()");
        return nullptr;
    }
    if (indexOf(id) < entries.size()) {
        const std::string_view idText = idTextOrNull(id);
        AERO_LOG_ERROR("editor: PanelRegistry::add: rejected duplicate panel id '{}'", idText);
        return nullptr;
    }

    entries.push_back(Entry{.panel = std::move(panel), .visible = true});
    return entries.back().panel.get();
}

void PanelRegistry::setVisible(const char* id, bool visible) {
    const std::size_t index = indexOf(id);
    if (index >= entries.size()) {
        const std::string_view idText = idTextOrNull(id);
        AERO_LOG_ERROR("editor: PanelRegistry::setVisible: unknown panel id '{}'", idText);
        return;
    }
    entries[index].visible = visible;
}

void PanelRegistry::toggle(const char* id) {
    const std::size_t index = indexOf(id);
    if (index >= entries.size()) {
        const std::string_view idText = idTextOrNull(id);
        AERO_LOG_ERROR("editor: PanelRegistry::toggle: unknown panel id '{}'", idText);
        return;
    }
    entries[index].visible = !entries[index].visible;
}

}  // namespace engine::editor

// editor/src/asset_picker_model.cpp -- task E.3.3 (D11): everything the picker decides. PURE: no
// ImGui, no entt, no GPU, no disk, no logging. The widget holds the glue and nothing else.
#include <aero/editor/asset_picker_model.hpp>
#include <aero/editor/project_files.hpp>  // leafOf -- the tree's own path helper, never re-derived

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace engine::editor {

bool assetPickerAccepts(const AssetPickerRules& rules, AssetKind kind) noexcept {
    // ONE LINE, AND IT MUST NEVER GROW A SECOND TERM. MP14 pins this identity over every kind and
    // every rules value the options array can produce, so the model cannot acquire a predicate of its
    // own and drift from the drop target that sits on the same button.
    return classifyAssetDrop(kind, rules.surface, /*targetHasMeshRenderer=*/false, rules.fieldKind) != DropAction::None;
}

std::vector<AssetKind> assetPickerFilterOptions(const AssetPickerRules& rules) {
    // Walks ASSET_KIND_FILTER_OPTIONS, never a list of its own, so enum order IS the combo's order by
    // construction and a kind added to the enum is offered here with no edit.
    std::vector<AssetKind> options;
    for (const AssetKind kind : ASSET_KIND_FILTER_OPTIONS) {
        if (assetPickerAccepts(rules, kind)) {
            options.push_back(kind);
        }
    }
    return options;
}

std::string assetPickerHeading(const AssetPickerRules& rules) {
    const std::vector<AssetKind> options = assetPickerFilterOptions(rules);
    if (options.empty()) {
        return {};
    }
    if (options.size() == 1) {
        // assetKindLabel VERBATIM -- the labels are SINGULAR and are never pluralised here. Appending
        // an 's' would answer "Audios" for AssetKind::Audio, which is why the heading is the label.
        return std::string(assetKindLabel(options.front()));
    }
    return "Any asset";
}

void buildAssetPickerCandidates(std::span<const AssetRecord> records, const AssetPickerRules& rules,
                                const AssetFilter& filter, std::size_t cap, AssetPickerCandidates& out) {
    out.items.clear();  // CLEAR, never shrink: the RenderViewScratch rule (MP8 pins the data pointer)
    out.total = 0;
    out.truncated = false;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const AssetRecord& record = records[index];
        if (!record.guid.valid()) {
            continue;  // no identity: never offerable, whatever else is true of it
        }
        const std::string_view leaf = leafOf(record.relativePath);
        const AssetKind kind = classifyAssetKind(leaf, /*isDirectory=*/false);
        // BOTH must hold, and neither is restated here: `rules` is the FIELD's law (the drop matrix)
        // and `filter` is the POPUP's combo and query (the browser's own text-and-kind predicate).
        if (!assetPickerAccepts(rules, kind) || !matchesFilter(leaf, /*isDirectory=*/false, filter)) {
            continue;
        }
        ++out.total;  // counted BEFORE the cap test -- `total` is never capped (the SearchResult rule)
        if (out.items.size() < cap) {
            // The kind is computed ONCE, here, and stored -- the widget must not recompute it per
            // frame per tile.
            out.items.push_back(AssetPickerCandidate{.guid = record.guid, .recordIndex = index, .kind = kind});
        }
    }
    out.truncated = out.total > out.items.size();
}

std::size_t movePickerCursor(std::size_t current, AssetPickerMove move, std::size_t candidateCount) noexcept {
    // TOTAL AND CLAMPING. The cursor is linear over [None, items...], so `last` is candidateCount --
    // index 0 is None and index i+1 is items[i]. An out-of-range `current` is clamped FIRST, which is
    // what makes a rebuild that shrank the list safe without the caller re-clamping.
    const std::size_t last = candidateCount;
    const std::size_t clamped = std::min(current, last);
    switch (move) {
        case AssetPickerMove::Prev:
            return clamped == 0 ? 0 : clamped - 1;
        case AssetPickerMove::Next:
            return clamped >= last ? last : clamped + 1;
        case AssetPickerMove::First:
            return 0;
        case AssetPickerMove::Last:
            return last;
    }
    return 0;  // unreachable; the switch is total over the enum and carries no default:
}

std::size_t initialPickerCursor(Guid current, const AssetPickerCandidates& candidates) noexcept {
    if (!current.valid()) {
        return ASSET_PICKER_NONE_INDEX;
    }
    for (std::size_t i = 0; i < candidates.items.size(); ++i) {
        if (candidates.items[i].guid == current) {
            return i + 1;  // +1, because index 0 is None
        }
    }
    return ASSET_PICKER_NONE_INDEX;  // bound to something this field cannot offer: land on None
}

std::optional<Guid> pickerCursorGuid(std::size_t cursor, const AssetPickerCandidates& candidates) noexcept {
    if (cursor == ASSET_PICKER_NONE_INDEX || cursor > candidates.items.size()) {
        return std::nullopt;  // None, and a past-the-end cursor means None too rather than a crash
    }
    return candidates.items[cursor - 1].guid;
}

AssetPickerAnchor assetPickerAnchor(Vec2 buttonMin, Vec2 buttonMax, Vec2 popupSize, Vec2 workMin,
                                    Vec2 workMax) noexcept {
    // BELOW is the answer, and everything after this is a correction to it.
    AssetPickerAnchor anchor{.pos = Vec2{buttonMin.x, buttonMax.y}, .pivot = Vec2{0.0F, 0.0F}, .above = false};

    // THE FINITENESS GUARD RUNS FIRST and returns that below-anchor with NO correction applied. Read
    // the header's contract before changing this: it is NOT a sanitiser, and a non-finite buttonMin.x
    // or buttonMax.y comes straight back out non-finite -- this function cannot invent a sane rect.
    // What it guarantees is ONE answer rather than a mixture, and that the corrections below never
    // manufacture a plausible finite position out of a garbage bound. It is also what keeps a future
    // fold into std::clamp safe, since std::clamp(NaN, lo, hi) returns NaN on libc++ (3.7.2's standing
    // rule) where std::min/std::max return the first argument.
    const bool finite = std::isfinite(buttonMin.x) && std::isfinite(buttonMin.y) && std::isfinite(buttonMax.x) &&
                        std::isfinite(buttonMax.y) && std::isfinite(popupSize.x) && std::isfinite(popupSize.y) &&
                        std::isfinite(workMin.x) && std::isfinite(workMin.y) && std::isfinite(workMax.x) &&
                        std::isfinite(workMax.y);
    if (!finite) {
        return anchor;
    }

    // FLIP ONLY WHEN THERE IS ROOM ABOVE. With room in neither direction the popup stays BELOW and
    // scrolls, which keeps the button itself visible -- flipping into a shorter gap would not.
    if (buttonMax.y + popupSize.y > workMax.y && buttonMin.y - popupSize.y >= workMin.y) {
        anchor.pos = Vec2{buttonMin.x, buttonMin.y};
        anchor.pivot = Vec2{0.0F, 1.0F};
        anchor.above = true;
    }

    // SHIFT LEFT, THEN FLOOR -- in that order. The floor is what keeps a popup wider than the work
    // area at the left edge rather than at a negative x.
    anchor.pos.x = std::min(anchor.pos.x, workMax.x - popupSize.x);
    anchor.pos.x = std::max(anchor.pos.x, workMin.x);

    // AND THE SAME RULE ON Y, which is not symmetry for its own sake. A popup TALLER than the work
    // area has room in neither direction, so it stays below -- and an API-set position is the one
    // ImGui never clamps (imgui.cpp:8279), so without this it is placed past the bottom of the screen,
    // where ImGui CULLS its grid child. A culled child makes ImGuiListClipper::Step() return false
    // immediately, so no tile is submitted, no thumbnail key is noted, and nothing in the picture says
    // why. MEASURED in a 320x180 window: the grid drew on the popup's appearing frame and on no frame
    // after it.
    //
    // With a (0,1) pivot the window's TOP is pos.y - size.y, so the clamp is applied to the effective
    // top and converted back -- never to `pos` directly, which means two different things.
    const float pivotOffset = anchor.above ? popupSize.y : 0.0F;
    float top = anchor.pos.y - pivotOffset;
    top = std::min(top, workMax.y - popupSize.y);
    top = std::max(top, workMin.y);
    anchor.pos.y = top + pivotOffset;
    return anchor;
}

AssetPickerLayout assetPickerLayout(const AssetPickerMetrics& metrics, bool withCombo) noexcept {
    // A NON-FINITE OR NON-POSITIVE METRIC YIELDS A FINITE, POSITIVE LAYOUT, because a NaN window size
    // is an ImGui assertion. The fallback font size is the one every other size is derived from.
    constexpr float FALLBACK_FONT = 16.0F;
    const auto sane = [](float value, float fallback) {
        return (std::isfinite(value) && value > 0.0F) ? value : fallback;
    };
    const float fontSize = sane(metrics.fontSize, FALLBACK_FONT);
    const float frameHeight = sane(metrics.frameHeight, fontSize);
    const float textLineHeight = sane(metrics.textLineHeight, fontSize);
    const float itemSpacingY =
        std::isfinite(metrics.itemSpacingY) && metrics.itemSpacingY >= 0.0F ? metrics.itemSpacingY : 0.0F;
    const float windowPadding =
        std::isfinite(metrics.windowPadding) && metrics.windowPadding >= 0.0F ? metrics.windowPadding : 0.0F;
    const float buttonWidth =
        std::isfinite(metrics.buttonWidth) && metrics.buttonWidth > 0.0F ? metrics.buttonWidth : 0.0F;

    // The tile's own geometry, exactly as drawContentsGrid computes it -- one tile is one tile
    // everywhere, so the grid child is an exact number of tile rows rather than a guess.
    const float tileEdge = fontSize * tileEdgeFontMultiple(TileSize::Small);
    const float pad = fontSize * TILE_CAPTION_PAD_FONT_MULTIPLE;
    const float captionH = static_cast<float>(TILE_CAPTION_LINES) * textLineHeight;
    const float tileH = tileEdge + captionH + (3.0F * pad);
    const float rowHeight = tileH + itemSpacingY;

    AssetPickerLayout layout;
    layout.gridHeight = static_cast<float>(ASSET_PICKER_GRID_ROWS_VISIBLE) * rowHeight;

    const float width = std::max(buttonWidth, ASSET_PICKER_MIN_WIDTH_FONT * fontSize);
    // heading + search (+ combo) + the grid + the ALWAYS-RESERVED notice line, plus the spacing
    // between them (four gaps without the combo, five with it).
    const float rows =
        frameHeight + frameHeight + (withCombo ? frameHeight : 0.0F) + layout.gridHeight + textLineHeight;
    const float gaps = itemSpacingY * (withCombo ? 5.0F : 4.0F);
    layout.popupSize = Vec2{width, (2.0F * windowPadding) + rows + gaps};
    return layout;
}

std::string assetPickerTruncationNotice(const AssetPickerCandidates& candidates) {
    if (!candidates.truncated) {
        return {};  // ALWAYS drawn, empty when nothing was truncated -- the line is reserved either way
    }
    return std::to_string(candidates.total - candidates.items.size()) + " more not shown -- refine the search";
}

std::string inspectorAssetFieldKey(std::string_view component, std::string_view field) {
    return std::string(component) + '.' + std::string(field);
}

std::string materialSlotFieldKey(std::size_t slot) { return "slot:" + std::to_string(slot); }

}  // namespace engine::editor

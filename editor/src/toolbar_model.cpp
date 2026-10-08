// Aero Engine — the toolbar's pure half (task E.6.2). No ImGui: see toolbar_model.hpp.
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/toolbar_model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace engine::editor {

namespace {

// A non-finite or non-positive UI scale is 1 -- copied, not shared, in the three layout TUs (the picking.cpp
// allFinite precedent): a one-line rule each, and none of them owns the others.
[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

[[nodiscard]] float sum(const auto& values) noexcept { return std::accumulate(values.begin(), values.end(), 0.0F); }

}  // namespace

std::string snapToggleTooltip(HostOs os) {
    std::string text = "Snap while dragging (hold ";
    text += modifierName(os);
    text += " to invert)";
    return text;
}

GizmoOperation snapFieldOperation(TransformTool tool) noexcept {
    switch (tool) {  // NO default:
        case TransformTool::Select:
        case TransformTool::Move:
            return GizmoOperation::Translate;
        case TransformTool::Rotate:
            return GizmoOperation::Rotate;
        case TransformTool::Scale:
            return GizmoOperation::Scale;
    }
    return GizmoOperation::Translate;
}

const char* snapStepFormat(TransformTool tool) noexcept {
    switch (snapFieldOperation(tool)) {  // NO default:
        case GizmoOperation::Translate:
            return "%.4g m";
        case GizmoOperation::Rotate:
            return "%.4g" AERO_GLYPH_DEGREE;
        case GizmoOperation::Scale:
            return "%.4g";
    }
    return "%.4g";
}

float snapStepValue(TransformTool tool, const SnapSettings& snap) noexcept {
    return snapStepFor(snapFieldOperation(tool), snap);
}

std::string snapStepText(TransformTool tool, const SnapSettings& snap) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), snapStepFormat(tool),
                                      static_cast<double>(snapStepValue(tool, snap)));
    return written > 0 ? std::string(buffer.data()) : std::string();
}

std::string undoAffordanceLabel(std::string_view undoLabel, float slotWidth, const TextWidth& measure) {
    if (undoLabel.empty()) {
        return {};
    }
    if (measure(undoLabel) <= slotWidth) {
        return std::string(undoLabel);
    }
    return elideCaptionRight(undoLabel,
                             [&measure, slotWidth](std::string_view line) { return measure(line) <= slotWidth; });
}

float shellBarHeight(float heightDp, float uiScale) noexcept {
    const float points = std::round(heightDp * layoutScale(uiScale));
    return std::isfinite(points) && points > 0.0F ? points : 0.0F;
}

ToolbarWidthPair toolbarWidths(const ToolbarMetrics& m) noexcept {
    const float twoPads = 2.0F * m.groupPadding;
    const float space = twoPads + m.localSegment + m.worldSegment;
    const float slot = UNDO_LABEL_SLOT_EM * m.fontSize;
    const float undoCompact = m.undoButton + m.itemSpacing + m.undoChip;
    return ToolbarWidthPair{
        .full = ToolbarWidths{.tools = twoPads + sum(m.toolFull),
                              .space = space,
                              .snap = twoPads + m.snapToggleFull + m.itemSpacing + m.snapField,
                              .play = sum(m.playFull) + (2.0F * m.itemSpacing),
                              .undo = undoCompact + m.itemSpacing + slot},
        .compact = ToolbarWidths{.tools = twoPads + sum(m.toolCompact),
                                 .space = space,
                                 .snap = twoPads + m.snapToggleCompact + m.itemSpacing + m.snapField,
                                 .play = sum(m.playCompact) + (2.0F * m.itemSpacing),
                                 .undo = undoCompact}};
}

ToolbarLayout toolbarLayout(float barWidth, const ToolbarWidths& full, const ToolbarWidths& compact,
                            float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const float width = (std::isfinite(barWidth) && barWidth > 0.0F) ? barWidth : 0.0F;
    const float pad = EDITOR_THEME.shell.toolbarPaddingX * s;
    const float gap = EDITOR_THEME.shell.toolbarGroupGap * s;
    const auto required = [pad, gap](const ToolbarWidths& w) {
        return pad + w.tools + gap + w.space + gap + w.snap + gap + w.play + gap + w.undo + pad;
    };
    ToolbarLayout out;
    if (required(full) <= width) {
        out.mode = ToolbarMode::Full;
    } else if (required(compact) <= width) {
        out.mode = ToolbarMode::Compact;
    } else {
        out.mode = ToolbarMode::Minimal;
    }
    const ToolbarWidths& w = out.mode == ToolbarMode::Full ? full : compact;
    out.toolsX = pad;
    out.spaceX = out.toolsX + w.tools + gap;
    out.snapX = out.spaceX + w.space + gap;
    out.leftGroupsEnd = out.snapX + w.snap;
    out.undoX = std::max(width - pad - w.undo, out.leftGroupsEnd + gap);
    out.playDrawn = out.mode != ToolbarMode::Minimal;
    if (out.playDrawn) {
        // Centred in [leftGroupsEnd + gap, undoX - gap]; the mode rule guarantees the span holds it.
        out.playX = (((out.leftGroupsEnd + gap) + (out.undoX - gap)) * 0.5F) - (w.play * 0.5F);
    }
    return out;
}

}  // namespace engine::editor

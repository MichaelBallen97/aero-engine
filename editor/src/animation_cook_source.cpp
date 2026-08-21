// Aero Engine — the ImportedModel -> animation cook adapter (task 3.5.2). See
// animation_cook_source.hpp for the contract, and for why targetNode is written through
// UNCONVERTED. PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/editor/animation_cook_source.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {
namespace {

[[nodiscard]] AnimationSourceResult fail(std::string error) {
    AnimationSourceResult out;
    out.ok = false;
    out.error = std::move(error);
    return out;
}

// The warning list is CAPPED and there is no uncapped total on this result, exactly as
// SkeletonSourceResult has none: the cook's own AnimationCookResult carries the capped-list plus
// uncapped-total shape, and duplicating it here would give the CLI two totals to reconcile. Every
// warning in this file goes through here so the cap cannot be forgotten at one site.
void addWarning(AnimationSourceResult& out, std::string text) {
    if (out.warnings.size() < MAX_IMPORT_WARNINGS) {
        out.warnings.push_back(std::move(text));
    }
}

// The two enum mirrors, each a TOTAL switch with no `default:` -- so a fourth enumerator on either
// side is a compile error here rather than a silent reinterpretation downstream. The trailing return
// is reachable in principle only because both enums have fixed underlying types (the
// cookedAnimationPathLabel precedent); no importer can produce a value that reaches it. The
// correspondence itself is asserted in the EDITOR test tier (AS4), the one place both enumerations
// are visible -- never in a header, so engine/assets stays free of any knowledge of /editor.
[[nodiscard]] assets::CookedAnimationPath cookedPath(AnimationPath path) noexcept {
    switch (path) {
        case AnimationPath::Translation:
            return assets::CookedAnimationPath::Translation;
        case AnimationPath::Rotation:
            return assets::CookedAnimationPath::Rotation;
        case AnimationPath::Scale:
            return assets::CookedAnimationPath::Scale;
    }
    return assets::CookedAnimationPath::Translation;
}

[[nodiscard]] assets::CookedAnimationInterpolation cookedInterpolation(AnimationInterpolation interpolation) noexcept {
    switch (interpolation) {
        case AnimationInterpolation::Linear:
            return assets::CookedAnimationInterpolation::Linear;
        case AnimationInterpolation::Step:
            return assets::CookedAnimationInterpolation::Step;
        case AnimationInterpolation::CubicSpline:
            return assets::CookedAnimationInterpolation::CubicSpline;
    }
    return assets::CookedAnimationInterpolation::Linear;
}

}  // namespace

AnimationSourceResult animationCookChannels(const ImportedModel& model, std::uint32_t clipIndex) {
    // 1. the clip itself. The message names what EXISTS, so "0 animation(s)" is the honest answer
    //    for a model that carries none rather than a different error for the same question.
    if (clipIndex >= model.animations.size()) {
        return fail(std::format("clip index {} is out of range: the model has {} animation(s)", clipIndex,
                                model.animations.size()));
    }
    const ImportedAnimation& clip = model.animations[clipIndex];

    AnimationSourceResult out;
    // 2. the multi-clip advisory. ONE home for it: the CLI relays what it finds here, so there is no
    //    second implementation to drift (a cooked clip is per-animation by construction).
    if (model.animations.size() > 1) {
        addWarning(
            out, std::format("the model has {} animations; cooking clip {} only", model.animations.size(), clipIndex));
    }

    // 3. the Structure-depth refusal, covering BOTH backends' shapes -- they differ, and a predicate
    //    that saw only one would let the other through. glTF records a Structure-depth channel with
    //    EMPTY times and values; FBX and Assimp push a channel only when it carries keys, so their
    //    Structure-depth clip is a shell with NO CHANNELS AT ALL. Neither shape can arise from a
    //    Full-depth import -- validateAccessor refuses a zero-count accessor and the FBX/Assimp
    //    helpers return early on an empty key list -- so this predicate is Structure's observable
    //    signature and the message says so by name. Cooking either would produce nothing rather than
    //    failing, which is the outcome a refusal exists to prevent.
    if (clip.channels.empty()) {
        return fail(
            std::format("clip {} carries no channels: this model was imported at Structure depth, "
                        "and a clip needs a Full import",
                        clipIndex));
    }
    bool anyKeys = false;
    for (const ImportedAnimationChannel& channel : clip.channels) {
        if (!channel.times.empty()) {
            anyKeys = true;
            break;
        }
    }
    if (!anyKeys) {
        return fail(
            std::format("clip {}'s {} channels all carry no keys: this model was imported at "
                        "Structure depth, and a clip needs a Full import",
                        clipIndex, clip.channels.size()));
    }

    // 4. the channels, in SOURCE ORDER. The COOK owns the canonical order, the duplicate
    //    (node, path) refusal and the drop of a keyless channel; this adapter converts nothing and
    //    sorts nothing, so the two layers' stories stay identical.
    out.channels.reserve(clip.channels.size());
    for (std::size_t c = 0; c < clip.channels.size(); ++c) {
        const ImportedAnimationChannel& channel = clip.channels[c];
        // DEFENCE IN DEPTH: every importer already skips a channel whose target node it could not
        // resolve, so this arm is unreachable from a document today. It is here because the sentinel
        // written through verbatim would become node id 4294967295, which binds to nothing and looks
        // exactly like a clip that simply does not animate that joint.
        if (channel.targetNode == INVALID_SUBASSET) {
            addWarning(out, std::format("channel {} names no target node and was dropped", c));
            continue;
        }
        AnimationSourceChannel emitted;
        // VERBATIM, and this is the line the whole header comment is about: a localId in, the same
        // localId out. NEVER a position, and never nodes[targetNode].
        emitted.targetLocalId = channel.targetNode;
        emitted.path = cookedPath(channel.path);
        emitted.interpolation = cookedInterpolation(channel.interpolation);
        emitted.times = channel.times;
        emitted.values = channel.values;
        out.channels.push_back(std::move(emitted));
    }

    out.ok = true;
    return out;
}

void animationCookChannelSpans(const AnimationSourceResult& source, std::vector<assets::AnimationCookChannel>& out) {
    out.clear();
    out.reserve(source.channels.size());
    for (const AnimationSourceChannel& channel : source.channels) {
        assets::AnimationCookChannel entry;
        entry.targetLocalId = channel.targetLocalId;
        entry.path = channel.path;
        entry.interpolation = channel.interpolation;
        entry.times = std::span<const float>(channel.times);
        entry.values = std::span<const Vec4>(channel.values);
        out.push_back(entry);
    }
}

assets::AnimationCookResult cookImportedAnimation(const ImportedModel& model, std::uint32_t clipIndex,
                                                  Guid sourceGuid) {
    AnimationSourceResult source = animationCookChannels(model, clipIndex);
    if (!source.ok) {
        assets::AnimationCookResult refused;
        refused.status = assets::AnimationCookStatus::Invalid;
        refused.message = std::move(source.error);
        return refused;
    }
    // The spans live HERE, in one frame, over storage that outlives them by construction -- the
    // whole reason animationCookChannelSpans exists as a function rather than as a field.
    std::vector<assets::AnimationCookChannel> spans;
    animationCookChannelSpans(source, spans);

    assets::AnimationCookInput input;
    input.sourceGuid = sourceGuid;
    input.sourceAnimationIndex = clipIndex;
    input.channels = spans;
    assets::AnimationCookResult out = assets::cookAnimation(input);
    // The adapter's advisories ride out FIRST: they are about the MODEL, and the cook's are about
    // the BYTES. One list, one printer, no second implementation. warningTotal is the UNCAPPED total
    // by its own definition, so it grows by what was merged in rather than staying the cook's alone.
    out.warnings.insert(out.warnings.begin(), source.warnings.begin(), source.warnings.end());
    out.warningTotal += source.warnings.size();
    return out;
}

}  // namespace engine::editor

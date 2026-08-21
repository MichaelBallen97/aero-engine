#pragma once
// Aero Engine — the ImportedModel -> animation cook adapter (task 3.5.2). The
// skeleton_cook_source.hpp mold, one format over: two pure functions plus one convenience, and no
// UI. No panel change, no menu item, no cook-on-import, no Library/ write.
//
// PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging -- warnings are RETURNED, never
// printed, because the CLI and a future editor cook path want them in different places.
//
// THIS IS THE FIFTH CONSUMER OF THE localId RULE (.claude/rules/editor.md) AND THE FIRST THAT MUST
// NOT CONVERT. Every previous consumer resolves a localId through a localId -> position map before
// indexing ImportedModel::nodes. This one indexes nothing: ImportedAnimationChannel::targetNode is a
// node localId -- a position for glTF, a raw ufbx typed_id for FBX, a walk-assigned id for Assimp --
// and it is written into the file as targetNodeLocalId VERBATIM, because .aeroskel's
// sourceNodeLocalId (docs/09 section 12.3) is the same kind of value and the two must be comparable
// at bind time. Mapping it to a position here would make every FBX clip bind to the wrong joints,
// silently. That inversion is the single easiest thing in this task to get wrong; AS9 is the case
// that reddens if anyone "fixes" it, and it is hand-built because glTF cannot see the difference.
#include <aero/assets/animation_cook.hpp>
#include <aero/editor/model_import.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

// OWNING, and containing NO span -- deliberately, so there is no lifetime rule to get wrong. The
// REJECTED shape is named here so it is not rediscovered: spans stored inside the result, pointing
// at the result's own vectors. It dangles on any copy, and it dangles DURING CONSTRUCTION the moment
// the outer vector reallocates, which is the harder of the two to see. This tree has shipped a
// dangling-view defect twice; a struct that cannot express one is worth more than a comment saying
// not to.
struct AnimationSourceChannel {
    std::uint32_t targetLocalId = 0;  // the SOURCE NODE's localId, UNCONVERTED (see above)
    assets::CookedAnimationPath path = assets::CookedAnimationPath::Translation;
    assets::CookedAnimationInterpolation interpolation = assets::CookedAnimationInterpolation::Linear;
    std::vector<float> times;
    std::vector<Vec4> values;
};

struct AnimationSourceResult {
    bool ok = false;
    std::string error;                             // "" IFF ok
    std::vector<AnimationSourceChannel> channels;  // OWNED; freely copyable and movable
    std::vector<std::string> warnings;             // capped at MAX_IMPORT_WARNINGS
};

// Walk `model` and produce the flat channel list cookAnimation consumes, for the clip at POSITION
// `clipIndex` in model.animations.
//
// Refusals name what exists: an out-of-range clip index reports the model's real animation count
// (zero included), and a Structure-depth model is refused BY NAME -- its channels carry empty times
// (glTF) or its clips carry no channels at all (FBX), and cooking either would silently produce
// nothing rather than failing.
[[nodiscard]] AnimationSourceResult animationCookChannels(const ImportedModel& model, std::uint32_t clipIndex);

// Builds the cook's span view over `source`, into caller-owned `out`. THE ONE PLACE a span into the
// adapter's storage is formed, so its lifetime is visible in one function rather than in a struct
// invariant. `source` must outlive every use of `out`; `out` is cleared and refilled.
void animationCookChannelSpans(const AnimationSourceResult& source, std::vector<assets::AnimationCookChannel>& out);

// animationCookChannels + animationCookChannelSpans + cookAnimation, the mesh_cook_source
// convenience shape: identical bytes to composing the three by hand, and never a second policy. An
// adapter error becomes {Invalid, error, {}, 0, {}}; adapter warnings ride out on the result, which
// is how the CLI reports them without a second implementation.
[[nodiscard]] assets::AnimationCookResult cookImportedAnimation(const ImportedModel& model, std::uint32_t clipIndex,
                                                                Guid sourceGuid);

}  // namespace engine::editor

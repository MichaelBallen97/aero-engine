#pragma once
// Aero Engine — engine::scene_audio (task 3.7.2): the World -> audio bridge. This is the ONLY code in
// the tree that sees BOTH engine::scene and engine::audio — it sits ABOVE both (docs/03), which is
// what keeps engine/audio scene-free and engine/scene audio-free. Two pieces, exactly the
// engine/scene_render shape one layer over:
//
//   * buildAudioView — a PURE free function that walks a World and resolves it into an AudioView: one
//     listener pose (lowest entity index, the Camera/DirectionalLight D5 rule) and one AudioSourceView
//     per PLAYING source, positions already in world space and every value already sanitised. It never
//     touches an AudioSystem, never resolves a Guid and never logs, which is what makes it tier-0
//     testable WITH NO AudioSystem IN EXISTENCE.
//   * SceneAudio — the facade: owns the entity <-> voice bindings and the coalescing state, and owns
//     NO AudioSystem, so a caller can drive several worlds through one system, or one world through
//     none.
//
// The component headers reach this file through <aero/scene/world.hpp>'s siblings in the .cpp only;
// this public header names World, Entity and audio types and nothing else.

#include <aero/audio/system.hpp>
#include <aero/scene/world.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::scene_audio {

// One resolved source, in WORLD space, with its Guid still UNRESOLVED. Deliberately NOT holding a
// VoiceHandle: buildAudioView is pure and knows nothing about voices, which is exactly what lets it be
// tested with no AudioSystem in existence.
struct AudioSourceView {
    Entity entity{};
    Guid clip{};
    audio::VoiceParams params{};  // position already world-space; volume/pitch already sanitised
};

// Reusable scratch storage so buildAudioView allocates nothing after warm-up (the vector keeps its
// capacity across calls) — the RenderViewScratch rule. Owned by the caller; SceneAudio keeps one.
struct AudioViewScratch {
    std::vector<AudioSourceView> sources;
};

struct AudioView {
    std::span<const AudioSourceView> sources;
    audio::ListenerPose listener{};   // listener.valid == false when the world has none
    std::uint32_t listenerCount = 0;  // >1 is legal; the LOWEST entity index wins (the D5 rule)
    std::uint32_t sourceCount = 0;    // == sources.size(); stated for symmetry with RenderView
};

// PURE and device-free, exactly as buildRenderView is. The returned view's span points INTO `scratch`,
// so a second call INVALIDATES a prior view. Non-const World& because World::each<> is non-const.
//
// ZERO LISTENERS leaves `valid = false`, `listenerCount = 0` and `volume` AT ITS DEFAULT 1.0F — which
// is what makes D23's "non-spatialized voices still play" true WITH NO SECOND BRANCH ANYWHERE IN THE
// MIXER, because the mixer multiplies every voice by listener.volume regardless.
//
// An entity with `playing == false` is NOT EMITTED, which is what makes SceneAudio::update's
// "no view entry => stop the voice" rule total, and what makes false -> true a retrigger.
[[nodiscard]] AudioView buildAudioView(World& world, AudioViewScratch& scratch);

class SceneAudio {
public:
    // In order, AND THE ORDER IS THE CONTRACT:
    //   1. system.service()                  -- reclaim finished slots BEFORE anything asks for one
    //   2. buildAudioView(world, scratch)
    //   3. system.setListener(view.listener) -- pushed unconditionally; it is one command
    //   4. reconcile bindings against view.sources
    //   5. drop bindings whose entity died, lost its AudioSource, or stopped playing
    void update(World& world, audio::AudioSystem& system);

    [[nodiscard]] std::size_t bindingCount() const noexcept;

    // COUNTED, NEVER WARNED -- the 3.1.5 rule verbatim: a nil or unregistered clip is the ORDINARY
    // state between authoring and loading, and a WARN would fire once per session on correct
    // behaviour.
    [[nodiscard]] std::uint32_t lastUnresolvedClips() const noexcept;
    [[nodiscard]] std::uint32_t lastListenerCount() const noexcept;

    // Stops every bound voice and forgets every binding.
    //
    // IT TAKES THE SYSTEM, AND THAT IS A CORRECTION rather than a convenience: SceneAudio owns no
    // AudioSystem, so a parameterless clear() could only FORGET -- and forgetting a looping voice's
    // binding leaves that voice playing for the rest of the system's life with nothing left that can
    // name it. The documented behaviour is the one worth having, so the signature is the one that can
    // deliver it.
    void clear(audio::AudioSystem& system) noexcept;

private:
    // One entity <-> voice binding. `lastPushed` is what makes the params COALESCING possible, and it
    // is compared as a WHOLE VoiceParams (its defaulted operator==) rather than field by field: a
    // field-by-field comparison is exactly where a future appended field gets forgotten.
    // THERE IS DELIBERATELY NO `finished` FLAG HERE, and its absence is a decision rather than an
    // omission. One was written in the first draft, set on the "voice is no longer playing" arm, and
    // READ NOWHERE: D11's no-restart behaviour is carried entirely by that arm's `continue`, so the
    // flag documented an intent it did not carry. A field that looks like state and is not is worse
    // than no field -- it invites the next reader to branch on it and to believe the branch means
    // something. If a future task needs "this one-shot has run its course" as data (a `finished`
    // observable, an animation-style event), that task adds it AND its consumer together.
    struct Binding {
        Entity entity{};
        audio::VoiceHandle voice{};
        Guid clip{};
        audio::VoiceParams lastPushed{};
    };

    // A std::vector SORTED BY THE WHOLE Entity -- index THEN generation, by the single `entityOrderLess`
    // in the .cpp that every sort and every search in this file goes through. Sorting by index ALONE
    // was this task's blocking review finding: step 4 matched by full equality and step 5 swept by
    // index, so a RECYCLED index orphaned a looping voice forever. One order, one rule.
    //
    // A std::vector rather than a node container is the AssetBindingTable decision, for the same two
    // reasons: std::unordered_map's move is not noexcept on MSVC's STL (the 3.1.2 R9 / C2607 rule),
    // and a few dozen entries binary-search faster than they hash.
    std::vector<Binding> bindings;
    AudioViewScratch scratch;
    std::vector<Entity> seen;  // scratch for step 5; kept so the sweep allocates nothing after warm-up

    std::uint32_t unresolvedClips = 0;
    std::uint32_t listenerCount = 0;

    // LATCHED, once per SceneAudio lifetime -- SceneRenderer's noCameraWarned / multiCameraWarned
    // members mirrored exactly.
    bool noListenerWarned = false;
    bool multiListenerWarned = false;
};

}  // namespace engine::scene_audio

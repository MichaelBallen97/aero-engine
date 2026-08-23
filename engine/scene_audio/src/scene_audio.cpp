// Aero Engine — the World -> audio bridge's implementation (task 3.7.2). Everything here runs on the
// GAME thread; nothing here touches the mixer directly, and nothing here allocates after warm-up
// except the binding vector's first growth.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene_audio/scene_audio.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace engine::scene_audio {
namespace {

// scene_renderer.cpp's own idiom, reused rather than reinvented.
[[nodiscard]] Vec3 translationOf(const Mat4& m) { return xyz(m.columns[3]); }

[[nodiscard]] float sanitizeFinite(float value, float fallback) {
    // The explicit finiteness arm rather than a bare clamp: libc++'s std::clamp(NaN, lo, hi) returns
    // NaN -- measured by task 3.6.3 and recorded.
    return std::isfinite(value) ? value : fallback;
}

// The one place a component's authored values become a VoiceParams. Clamped and sanitised HERE, so
// nothing downstream has to wonder whether it was.
[[nodiscard]] audio::VoiceParams paramsFrom(const AudioSource& source, Vec3 worldPosition) {
    audio::VoiceParams params;
    params.position = Vec3{sanitizeFinite(worldPosition.x, 0.0F), sanitizeFinite(worldPosition.y, 0.0F),
                           sanitizeFinite(worldPosition.z, 0.0F)};
    params.volume = std::clamp(sanitizeFinite(source.volume, 1.0F), 0.0F, 1.0F);
    params.pitch = std::clamp(sanitizeFinite(source.pitch, 1.0F), audio::MIN_PITCH, audio::MAX_PITCH);
    params.minDistance = sanitizeFinite(source.minDistance, 0.0F);
    params.maxDistance = sanitizeFinite(source.maxDistance, 0.0F);
    params.loop = source.loop;
    params.spatialize = source.spatialize;
    return params;
}

void warnOnce(bool& latch, const char* message) {
    if (!latch) {
        latch = true;
        AERO_LOG_WARN("{}", message);
    }
}

}  // namespace

AudioView buildAudioView(World& world, AudioViewScratch& scratch) {
    AERO_PROFILE_ZONE_NAMED("scene_audio.buildAudioView");

    scratch.sources.clear();  // KEEPS CAPACITY -- the RenderViewScratch rule; nothing here allocates
                              // after warm-up.

    AudioView view;

    // ---- the listener ---------------------------------------------------------------------------
    // The LOWEST ENTITY INDEX wins (the Camera / DirectionalLight D5 rule, so a reader who knows one
    // of the three knows all three). World::each's iteration order is deterministic but NOT ordered,
    // so the winner is selected by comparison rather than by taking the first.
    Entity listenerEntity{};
    float listenerVolume = 1.0F;
    std::uint32_t listeners = 0;
    world.each<Transform, AudioListener>([&](Entity e, Transform&, AudioListener& listener) {
        ++listeners;
        if (!listenerEntity.valid() || e.index < listenerEntity.index) {
            listenerEntity = e;
            listenerVolume = std::clamp(sanitizeFinite(listener.volume, 1.0F), 0.0F, 1.0F);
        }
    });
    view.listenerCount = listeners;

    if (listenerEntity.valid()) {
        const Mat4 m = worldMatrix(world, listenerEntity);
        view.listener.position = translationOf(m);
        // NORMALISING THE COLUMNS is what makes a SCALED listener entity behave identically to an
        // unscaled one. normalizeOrZero, never normalize: a degenerate zero-scale column yields a zero
        // axis and the pan collapses to CENTRE, where normalize() would assert in Debug and produce a
        // NaN in Release (its contract is an assert-and-no-branch).
        view.listener.right = normalizeOrZero(xyz(m.columns[0]));
        view.listener.up = normalizeOrZero(xyz(m.columns[1]));
        view.listener.forward = normalizeOrZero(-xyz(m.columns[2]));  // -Z forward, ADR-005
        view.listener.volume = listenerVolume;
        view.listener.valid = true;
    }
    // ZERO LISTENERS leaves the pose default-constructed: valid == false AND volume STILL 1.0F, which
    // is what makes D23's "non-spatialized voices still play" true with no second branch in the mixer.

    // ---- the sources ----------------------------------------------------------------------------
    world.each<Transform, AudioSource>([&](Entity e, Transform&, AudioSource& source) {
        if (!source.playing) {
            return;  // NOT EMITTED -- which is what makes update()'s "no view entry => stop" total
        }
        const Vec3 sourcePosition = translationOf(worldMatrix(world, e));
        scratch.sources.push_back(
            AudioSourceView{.entity = e, .clip = source.clip, .params = paramsFrom(source, sourcePosition)});
    });

    view.sources = std::span<const AudioSourceView>{scratch.sources};
    view.sourceCount = static_cast<std::uint32_t>(scratch.sources.size());
    return view;
}

void SceneAudio::update(World& world, audio::AudioSystem& system) {
    AERO_PROFILE_ZONE_NAMED("scene_audio.update");

    // 1. RECLAIM FINISHED SLOTS BEFORE ANYTHING ASKS FOR ONE. Reconciling first would make a
    //    same-frame retrigger fail to find a slot on a world that is at the cap.
    system.service();

    // 2.
    const AudioView view = buildAudioView(world, scratch);

    // 3. Pushed unconditionally; it is ONE command, and the ring's reserve is sized for exactly this.
    system.setListener(view.listener);

    listenerCount = view.listenerCount;
    if (view.listenerCount == 0) {
        warnOnce(noListenerWarned, "SceneAudio: no AudioListener in world; spatialized sources are silent");
    } else if (view.listenerCount > 1) {
        warnOnce(multiListenerWarned, "SceneAudio: multiple AudioListeners; using lowest entity index");
    }

    // 4. Reconcile.
    unresolvedClips = 0;
    seen.clear();
    for (const AudioSourceView& source : view.sources) {
        const auto position = std::lower_bound(
            bindings.begin(), bindings.end(), source.entity,
            [](const Binding& binding, const Entity& key) { return binding.entity.index < key.index; });
        const bool bound = position != bindings.end() && position->entity == source.entity;

        // A nil clip and an unregistered clip are the SAME ordinary in-flight state: COUNTED, never
        // warned. Note the order -- an unresolvable clip drops any binding it had, so a clip cleared
        // to nil stops the voice rather than leaving it playing forever.
        const audio::ClipHandle clip = system.findClip(source.clip);
        if (!clip.valid()) {
            ++unresolvedClips;
            if (bound) {
                system.stop(position->voice);
                bindings.erase(position);
            }
            continue;
        }

        seen.push_back(source.entity);

        if (!bound) {
            const audio::VoiceHandle voice = system.play(clip, source.params);
            if (!voice.valid()) {
                // Record NOTHING and let the next frame retry: a slot may free between frames, and a
                // half-recorded binding would make the retry impossible.
                continue;
            }
            bindings.insert(position, Binding{.entity = source.entity,
                                              .voice = voice,
                                              .clip = source.clip,
                                              .lastPushed = source.params,
                                              .finished = false});
            continue;
        }

        if (position->clip != source.clip) {
            // A CLIP SWAP IS A RESTART: stop the old voice and start the new one.
            system.stop(position->voice);
            const audio::VoiceHandle voice = system.play(clip, source.params);
            if (!voice.valid()) {
                bindings.erase(position);
                continue;
            }
            position->voice = voice;
            position->clip = source.clip;
            position->lastPushed = source.params;
            position->finished = false;
            continue;
        }

        if (!system.isPlaying(position->voice)) {
            // A FINISHED NON-LOOPING VOICE IS NOT RESTARTED (D11). `playing` is authored state; the
            // component says "this should be playing", and a one-shot that has run its course has.
            // Retrigger is false -> true, which drops the binding at step 5 and takes the play() arm
            // on the next update.
            position->finished = true;
            continue;
        }

        if (position->lastPushed != source.params) {
            // The comparison is over the WHOLE VoiceParams, never field by field.
            system.setParams(position->voice, source.params);
            position->lastPushed = source.params;
        }
        // Otherwise NOTHING AT ALL. That coalescing is not an optimisation -- it is what makes the
        // command ring's capacity sound: without it a 64-source scene at 1000 fps produces 65 000
        // commands per second against a drain rate of roughly one ring's worth per callback.
    }

    // 5. Drop every binding whose entity is absent from the view -- dead, or `playing` went false, or
    //    the component was removed, or its clip stopped resolving. A DROPPED BINDING IS HOW
    //    false -> true BECOMES A RETRIGGER.
    std::sort(seen.begin(), seen.end(), [](const Entity& a, const Entity& b) { return a.index < b.index; });
    std::size_t write = 0;
    for (std::size_t read = 0; read < bindings.size(); ++read) {
        const Binding& binding = bindings[read];
        const auto byIndex = [](const Entity& a, const Entity& b) { return a.index < b.index; };
        const bool present = std::binary_search(seen.begin(), seen.end(), binding.entity, byIndex);
        if (present) {
            if (write != read) {
                bindings[write] = bindings[read];
            }
            ++write;
        } else {
            system.stop(binding.voice);
        }
    }
    bindings.resize(write);
}

std::size_t SceneAudio::bindingCount() const noexcept { return bindings.size(); }

std::uint32_t SceneAudio::lastUnresolvedClips() const noexcept { return unresolvedClips; }

std::uint32_t SceneAudio::lastListenerCount() const noexcept { return listenerCount; }

void SceneAudio::clear(audio::AudioSystem& system) noexcept {
    for (const Binding& binding : bindings) {
        system.stop(binding.voice);
    }
    bindings.clear();
    seen.clear();
    unresolvedClips = 0;
    listenerCount = 0;
    // The two WARN latches are deliberately NOT reset: they are once per SceneAudio LIFETIME, not
    // once per world, and resetting them here would turn a per-frame clear() into a per-frame WARN.
}

}  // namespace engine::scene_audio

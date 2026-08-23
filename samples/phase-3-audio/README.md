# phase-3-audio — task 3.7.2's deliverable

The first noise this engine has ever made. A real `engine::World` with three entities, driven through
`engine::scene_audio::SceneAudio` every frame:

| entity | components |
|---|---|
| `listener` | `Transform` at the origin + `AudioListener{volume 1.0}` |
| `orbit` | `Transform` on a radius-3 circle + `AudioSource{orbit.aerowave, loop, spatialize, minDistance 1, maxDistance 12}` |
| `beacon` | `Transform` at `(0, 0, -10.8)` + `AudioSource{beacon.aerowave, …}` |

The beacon sits at **90 % of `maxDistance`** — audible but faint — so one run demonstrates the pan and
the rolloff together.

## Running it

```bash
build/<preset>/samples/phase-3-audio/aero_sample_phase3_audio            # device mode, 8 s
build/<preset>/samples/phase-3-audio/aero_sample_phase3_audio --seconds 4 --period 2
build/<preset>/samples/phase-3-audio/aero_sample_phase3_audio --dump-pcm /tmp/orbit.pcm --seconds 8
```

| flag | default | what it does |
|---|---|---|
| `--seconds N` | 8 | how long to run |
| `--period N` | 4 | one full orbit, in seconds |
| `--pitch F` | 1.0 | applied to the **orbiting** source only, so the pitch validation row needs no code change |
| `--dump-pcm <path>` | — | **implies no device and no thread**; writes raw `f32le`, 48 kHz, 2 ch |
| `--no-spatialize` | — | the A/B twin: both sources go 2D |
| `--no-loop` | — | both sources become one-shots |

**The device mode is the default, and `--dump-pcm` implies no device.** That is stated here because
"why did my dump run also make a noise" is otherwise a five-minute mystery.

A dump is exactly `N × 48000 × 2 × 4` bytes, and it is **byte-reproducible**: the dump loop advances
the orbit by a **fixed** `dt` of `512 / 48000` s per block rather than by wall clock. Two runs with
identical flags produce identical bytes; `shasum -a 256` is the check.

**The sample prints its own expected table at startup**, computed from `audio::distanceGain` and
`audio::panGains` *themselves* rather than from any number this file predicts. Compare a measurement
against what that build printed, never against a document.

## The two committed fixtures

Both **mono**, 48 kHz, 0.5 s, **48 064 bytes each** — `64 + 2 × 1 × 24000`, with no padding term
anywhere, because `.aerowave` has zero padding sites and that is a contract.

| file | signal | frames | why |
|---|---|---|---|
| `orbit.aerowave` | 480 Hz sine | 24 000 | 480 Hz at 48 kHz is **exactly 100 samples per cycle**, so 24 000 frames is **exactly 240 whole cycles** and the loop seam is continuous *by arithmetic* rather than by listening |
| `beacon.aerowave` | 240 Hz sine | 24 000 | exactly 120 whole cycles, an octave below — the two are unmistakable by ear as well as by spectrum |

Both are **mono on purpose**: a spatialized source is downmixed to the arithmetic mean before panning,
so a stereo fixture would be a poor witness for this sample's own feature.

### Regenerating them

Byte-reproducible — this is the cross-lane guarantee the determinism work bought, being *spent* rather
than re-proven. **Count the GUID digits: each is 32 hex characters.** A 31-character GUID parses to
nil and looks right while being wrong.

```bash
cd samples/phase-3-audio
ffmpeg -f lavfi -i "sine=frequency=480:sample_rate=48000:duration=0.5" -ac 1 -c:a pcm_s16le orbit.wav
ffmpeg -f lavfi -i "sine=frequency=240:sample_rate=48000:duration=0.5" -ac 1 -c:a pcm_s16le beacon.wav
"$B"/tools/cooker/aero_cooker audio --input orbit.wav  --output orbit.aerowave  \
    --guid 37200000000000000000000000000001
"$B"/tools/cooker/aero_cooker audio --input beacon.wav --output beacon.aerowave \
    --guid 37200000000000000000000000000002
stat -f%z orbit.aerowave beacon.aerowave    # BOTH must read exactly 48064
rm -f orbit.wav beacon.wav                  # the .wav sources are NOT committed
```

**Neither fixture enters `tests/cooker/determinism.sha256`.** They are wav-sourced and therefore
*eligible*, but the manifest is frozen at five arms and this task adds no cook path — and a sample's
own asset is not a cooker regression witness. Stated here so nobody adds them "for completeness".

## The lifetime rule, which this sample makes physical

**Declare the device AFTER the system.** `ma_device_uninit` stops the stream and joins the audio thread
before it returns, so ordinary reverse-order destruction tears the device down first, every time, with
no flag and no handshake. `main.cpp` declares them in that order with the rule beside them, and
`system.hpp` and `audio.hpp` both carry it too — it is the one rule a consumer can get wrong, and the
symptom is a use-after-free on a thread nobody is looking at.

## What CI does with this

It **builds** it on three lanes and never runs it: no lane has audio hardware, and the null backend
makes no sound. The audible half is what the macOS validation pass is for.

# `tests/fixtures/audio/` — the audio fixture set and its external anchor (task 3.7.1)

One source signal, four encodings, one external decode. Everything here is **committed**, so no CI
lane and no other machine ever needs ffmpeg — the same posture `samples/phase-3-materials/textures/`
takes for its cooked `.ktx2` files, one step further: here the external tool produces the **anchor**
as well as the inputs.

- **Tool:** ffmpeg 8.1.2, `/opt/homebrew/bin/ffmpeg` (Homebrew).
- **Date:** 2026-08-22.
- Every command below was run from this directory.

## The commands

```bash
# 1. the source signal -> the wav fixture (16-bit, mono, 8 kHz, 0.25 s = 2000 frames)
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=0.25" \
       -af "volume=0.8" -c:a pcm_s16le -ac 1 tone.wav

# 2. the three other encodings, all FROM tone.wav so every one carries the same signal
ffmpeg -i tone.wav -c:a flac                     tone.flac
ffmpeg -i tone.wav -c:a libmp3lame -b:a 128k     tone.mp3
ffmpeg -i tone.wav -c:a vorbis -strict -2 -q:a 5 -ac 2 tone.ogg   # -ac 2 is FORCED; see below

# 3. THE EXTERNAL ANCHOR: ffmpeg's own decode, raw interleaved s16 LE, no container
ffmpeg -i tone.wav -f s16le -acodec pcm_s16le tone.s16le.pcm
```

`tone.aerowave` is **not here yet**. It is the loader golden, cooked once by the real `aero_cooker`
binary and then frozen, and that binary does not exist until later in this task. It arrives with the
CLI, and its exact invocation and pinned GUID are recorded here on the day it lands.

## What was measured, rather than assumed

| File | Bytes | Codec | Rate | Ch | Duration | ffmpeg's own decode | Peak \|sample\| |
|---|---|---|---|---|---|---|---|
| `tone.wav` | 4078 | pcm_s16le | 8000 | 1 | 0.250000 s | 2000 frames | 3276 |
| `tone.flac` | 9281 | flac | 8000 | 1 | 0.250000 s | 2000 frames | 3276 |
| `tone.mp3` | 4653 | mp3 | 8000 | 1 | 0.250000 s | 2000 frames | 3113 |
| `tone.ogg` | 4161 | vorbis | 8000 | **2** | 0.256000 s | **1024 frames** | 2341 |
| `tone.s16le.pcm` | **4000** | — (raw) | 8000 | 1 | — | — | 3276 |

SHA-256:

```
ff06e9695769ee8d865d2a2746e508fe3a1f7b24a2a7afb278fed41967640aa8  tone.wav
987122a5a42e9717b2e2432c5135dd66c6511af0f8bfc31564201258981b7411  tone.flac
157c5e2f8cb000c98b51e60ca0ac34b5e1f10baed7305e245e07a9a5b8be2155  tone.mp3
6130072d5a65b1ea4d5e002dd9d242eea2f189a9867a546b155b03d822d22433  tone.ogg
716a5886e71ebf9b86eb8991bfe41a6abd1aab915d527a41807a93e41c877c68  tone.s16le.pcm
```

**`tone.s16le.pcm` is exactly 4000 bytes** — 2000 frames × 1 channel × 2 B — which is the number every
derived size in this task rests on, including `tone.aerowave`'s 4064 (a 64-byte header plus the same
4000 bytes of samples). It was verified with `wc -c` before anything downstream was written, because a
`sine` filter that emitted 2001 frames on some other ffmpeg build would poison every size in the task.
If it is ever regenerated and comes out a different length, **record the measured frame count and
re-derive from it — never trim the file by hand.**

## The two properties that make these fixtures load-bearing

**1. `tone.wav`, `tone.flac` and `tone.s16le.pcm` agree byte for byte after decode**, and a test
asserts it. Verified here with ffmpeg on both sides:

```bash
ffmpeg -i tone.flac -f s16le -acodec pcm_s16le /tmp/flac-decode.pcm
cmp /tmp/flac-decode.pcm tone.s16le.pcm     # → identical, 4000 bytes
```

That is genuine cross-implementation agreement — dr_wav and dr_flac against libavcodec — and this
project's own parser cannot fake it, because nothing of ours produced the anchor. **If it ever
reddens, dr_wav or dr_flac has changed behaviour, which is exactly what the anchor exists to catch.
Understand the difference and record it; do not regenerate the golden from our own output.**

**2. `tone.mp3` and `tone.ogg` have no byte golden, by design.** Both are lossy, so ffmpeg's decode
and dr_mp3's / stb_vorbis's decode are *supposed* to differ, and neither ever enters
`tests/cooker/determinism.sha256`. Their tests assert sample rate, channel count, a frame count
within tolerance and a peak amplitude in the right neighbourhood — **never a digest**.

**The mp3 frame-count tolerance, stated rather than discovered.** MP3 carries encoder delay and
padding, so a decoder's frame count legitimately differs from the source's by up to one or two
granules. The test asserts `|decoded − 2000| <= 4608` (four granules). ffmpeg's own decode of
`tone.mp3` reads exactly 2000 frames here, because it honours the LAME/Xing gapless header; dr_mp3
does not necessarily, which is what the tolerance is for.

## Two things ffmpeg would not do, recorded rather than worked around

**(a) The ogg fixture cannot be mono.** This ffmpeg build carries only the *native, experimental*
Vorbis encoder (`vorbis`, flagged `X`); it is not built against `libvorbis`, and there is no `oggenc`
on this machine. The native encoder refuses anything but stereo:

```
[vorbis @ …] Current FFmpeg Vorbis encoder only supports 2 channels.
```

So `tone.ogg` alone carries `-ac 2`, and it is the one fixture here that is **not** mono. Both of its
channels hold the same signal, since the source is mono.

**(b) At 2000 frames the ogg loses roughly half the signal.** 2000 frames is shorter than Vorbis's
2048-sample long block, so the encoder emits a **single** packet and the file decodes to **1024**
frames rather than 2000 (`ffprobe -count_frames` reads `nb_read_frames=1`, and the stream's own
`duration_ts` is 2048). This is a property of the short, low-rate source, not of the file being
damaged: the identical command on a 1.0 s / 8000-frame source round-trips to exactly 8000 frames.

`tone.ogg` is still a valid Vorbis stream and is still the right fixture for proving the stb_vorbis
backend decodes one — but **its frame count and channel count are its own, not the source's**, and any
test or validation row that compares an ogg reading against 2000 frames or 1 channel is comparing
against the wrong numbers. The measured values are in the table above.

If a mono, full-length ogg is ever wanted, it needs an encoder this machine does not have — an ffmpeg
built with `--enable-libvorbis`, or `oggenc` from vorbis-tools — or a longer source signal, which
would move `tone.s16le.pcm` off 4000 bytes and every size derived from it.

## The signal

440 Hz sine, 8 kHz, mono, 0.25 s, at `volume=0.8`. **The measured peak is 3276, not the ~26214 that
"80 % of full scale" would suggest**: ffmpeg's lavfi `sine` source emits at amplitude **0.125**
(4095/32767, measured by generating the same signal with no `volume` filter), so `volume=0.8` lands at
0.100 of full scale, about −20 dBFS. Any "peak amplitude in the right neighbourhood" assertion belongs
against **3276**, and against the per-format peaks in the table for the two lossy encodings.

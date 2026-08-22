# `tests/fixtures/audio/` — the audio fixture set and its external anchor (task 3.7.1)

One source signal, four encodings, one external decode. Everything here is **committed**, so no CI
lane and no other machine ever needs ffmpeg — the same posture `samples/phase-3-materials/textures/`
takes for its cooked `.ktx2` files, one step further: here the external tool produces the **anchor**
as well as the inputs.

- **Tool:** ffmpeg 8.1.2, `/opt/homebrew/bin/ffmpeg` (Homebrew).
- **Date:** 2026-08-23.
- Every command below was run from this directory.

## The commands

```bash
# 1. the source signal -> the wav fixture (16-bit, mono, 8 kHz, 1.0 s = 8000 frames)
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=1.0" \
       -af "volume=0.8" -c:a pcm_s16le -ac 1 tone.wav

# 2. the three other encodings, all FROM tone.wav so every one carries the same signal
ffmpeg -i tone.wav -c:a flac                     tone.flac
ffmpeg -i tone.wav -c:a libmp3lame -b:a 128k     tone.mp3
ffmpeg -i tone.wav -c:a vorbis -strict -2 -q:a 5 -ac 2 tone.ogg   # -ac 2 is FORCED; see below

# 3. THE EXTERNAL ANCHOR: ffmpeg's own decode, raw interleaved s16 LE, no container
ffmpeg -i tone.wav -f s16le -acodec pcm_s16le tone.s16le.pcm
```

## `tone.aerowave` — the loader golden, cooked once and FROZEN

`tone.aerowave` is not an ffmpeg product. It is the artifact `audio::loadAudioClip` reads end to end,
cooked **once** by the real `aero_cooker` binary on 2026-08-23 and frozen from that moment:

```bash
# run from THIS directory, against a Release build of the cooker
../../../build/macos-release/tools/cooker/aero_cooker audio \
    --input tone.wav --output tone.aerowave \
    --guid 0123456789abcdeffedcba9876543210
wc -c tone.aerowave        # -> exactly 16064  ==  64 + 2 x 1 x 8000
```

The GUID is the tree's **standard test GUID** — the same all-nonzero value `run_case.cmake`'s
`TEST_GUID` uses. Every one of its sixteen bytes is non-zero, which is what makes an assertion against
it a statement about byte **order** rather than merely about presence: it lands in the artifact as
`efcdab89674523011032547698badcfe` at offset 16, hi then lo, each little-endian.

**FROZEN means frozen.** If this file ever has to change, the format changed, and that is a
`formatVersion` decision recorded in `docs/09-file-formats.md` section 14 — never a fixture edit made
to green a red run. Its sample region is byte-identical to `tone.s16le.pcm`, verified at the moment it
was cooked:

```bash
tail -c 16000 tone.aerowave | cmp - tone.s16le.pcm    # -> identical
```

so the same external anchor that ties the decode also ties this artifact's bulk region, and the only
bytes in it that no third party has ever seen are the 64 header bytes.

## Why the source is 1.0 s, and not the 0.25 s it was first cut at

The first cut of this set used a 0.25 s source, and **the ogg silently lost roughly half the signal**:
2000 frames is shorter than Vorbis's 2048-sample long block, so the encoder emitted a single packet
and the file decoded back to **1024** frames. That is a real property of a short, low-rate source
rather than a damaged file, but it reads as a decoder defect to anyone opening the test later, and it
would have forced every ogg assertion in the task onto numbers that agree with no other fixture.

All four candidate lengths were measured before choosing:

| Source duration | wav frames | ogg frames after round trip |
|---|---|---|
| 0.25 s | 2000 | **1024** (loses 976, ~49 %) |
| 0.50 s | 4000 | 4096 (+96) |
| **1.00 s** | **8000** | **8000 — EXACT** |
| 2.00 s | 16000 | 16000 (exact) |

**1.0 s is the shortest length at which the Vorbis round trip is frame-exact**, so all four encodings
decode to the same 8000-frame signal and every number in the task comes off one arithmetic.

## What was measured, rather than assumed

| File | Bytes | Codec | Rate | Ch | Duration | ffmpeg's own decode | Peak \|sample\| |
|---|---|---|---|---|---|---|---|
| `tone.wav` | 16078 | pcm_s16le | 8000 | 1 | 1.000000 s | 8000 frames | 3276 |
| `tone.flac` | 12098 | flac | 8000 | 1 | 1.000000 s | 8000 frames | 3276 |
| `tone.mp3` | 10413 | mp3 | 8000 | 1 | 1.000000 s | 8000 frames | 3113 |
| `tone.ogg` | 4968 | vorbis | 8000 | **2** | 1.000000 s | **8000 frames** | 2340 |
| `tone.s16le.pcm` | **16000** | — (raw) | 8000 | 1 | — | — | 3276 |
| `tone.aerowave` | **16064** | — (cooked) | 8000 | 1 | 1.000000 s | — | 3276 |

SHA-256:

```
e29cde9ba09ca2ed1c925fcf1145a75dd8e3622b8ab3a3a53d50a5ea5d469aa7  tone.wav
bfcd8acace1c26a8cc1051ac369eccd5e12c3842dccf6112f78491e395dec52b  tone.flac
b60068eaa1cac8cdae3fbdca65fa82cd067b9c092a5e82f0581a5d07bb2788a6  tone.mp3
650127c34da6076f8cad3b43aec85f28e33c7004b1907c027b8525ce09b8b9d5  tone.ogg
5dcb67dd71c9135f51d224ca7a703e149eabc6c79f4380a95bc846a2d6d402bf  tone.s16le.pcm
f6ba2b2b0585c8e2325fb003af8dd4439d9ef4350e2697806762cc9293dbb40d  tone.aerowave
```

**`tone.s16le.pcm` is exactly 16000 bytes** — 8000 frames × 1 channel × 2 B — which is the number
every derived size in this task rests on, including `tone.aerowave`'s 16064 (a 64-byte header plus the
same 16000 bytes of samples). It was verified with `wc -c` before anything downstream was written,
because a `sine` filter that emitted 8001 frames on some other ffmpeg build would poison every size in
the task. If it is ever regenerated and comes out a different length, **record the measured frame
count and re-derive from it — never trim the file by hand.**

## The two properties that make these fixtures load-bearing

**1. `tone.wav`, `tone.flac` and `tone.s16le.pcm` agree byte for byte after decode**, and a test
asserts it. Verified here with ffmpeg on both sides:

```bash
ffmpeg -i tone.flac -f s16le -acodec pcm_s16le /tmp/flac-decode.pcm
cmp /tmp/flac-decode.pcm tone.s16le.pcm     # -> identical, 16000 bytes
ffmpeg -i tone.wav  -f s16le -acodec pcm_s16le /tmp/wav-decode.pcm
cmp /tmp/wav-decode.pcm  tone.s16le.pcm     # -> identical, 16000 bytes
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
granules. The test asserts `|decoded − 8000| <= 4608` (four granules). At the 1.0 s length that is
genuine, meaningful slack — **±58 % of the signal** — where at the earlier 0.25 s length the same
four-granule window was ±230 % of a 2000-frame clip and could not have failed for any decode short of
a total refusal. ffmpeg's own decode of `tone.mp3` reads exactly 8000 frames here, because it honours
the LAME/Xing gapless header; dr_mp3 does not necessarily, which is what the tolerance is for.

## Two things ffmpeg would not do, recorded rather than worked around

**(a) The ogg fixture cannot be mono.** This ffmpeg build carries only the *native, experimental*
Vorbis encoder (`vorbis`, flagged `X`); it is not built against `libvorbis`, and there is no `oggenc`
on this machine. The native encoder refuses anything but stereo, re-confirmed after the regeneration:

```
[vorbis @ ...] Current FFmpeg Vorbis encoder only supports 2 channels.
```

So `tone.ogg` alone carries `-ac 2`, and it is the one fixture here that is **not** mono. Both of its
channels hold the same signal, since the source is mono.

**That is an upside rather than a limitation, and it is the only one of its kind in this set.** A
1-channel fixture cannot witness a channel-major interleaving defect at all — with one channel,
frame-major and channel-major are the same byte order — so `tone.ogg` is the only committed source
here whose decode can see one. The tests that pin interleaving order do so on hand-built multi-channel
data as well, but the ogg arm is the only place a *real decoder's* channel order is observed.

**(b) The ogg's channel count is its own, not the source's.** Any test or validation row that compares
an ogg reading against **1** channel is comparing against the wrong number: the ogg arm asserts
**2 channels and 8000 frames**. Its frame count now matches the other three exactly; only the channel
count differs.

If a mono ogg is ever wanted, it needs an encoder this machine does not have — an ffmpeg built with
`--enable-libvorbis`, or `oggenc` from vorbis-tools.

## The signal

440 Hz sine, 8 kHz, mono, 1.0 s, at `volume=0.8`. **The measured peak is 3276, not the ~26214 that
"80 % of full scale" would suggest**: ffmpeg's lavfi `sine` source emits at amplitude **0.125**
(4095/32767, measured by generating the same signal with no `volume` filter), so `volume=0.8` lands at
0.100 of full scale, about −20 dBFS. Any "peak amplitude in the right neighbourhood" assertion belongs
against **3276**, and against the per-format peaks in the table for the two lossy encodings.

The anchor's first four samples are `0, 1110, 2088, 2820` and its last four are
`-3218, -2820, -2089, -1110`, measured from `tone.s16le.pcm` itself — a first-and-last-sample
assertion can be written against those literals.

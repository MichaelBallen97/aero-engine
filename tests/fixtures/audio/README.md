# `tests/fixtures/audio/` — the audio fixture set and its external anchor (task 3.7.1)

One source signal, four encodings, one external decode, and one deliberately corrupt derivative.
Everything here is **committed**, so no CI lane and no other machine ever needs ffmpeg — the same
posture `samples/phase-3-materials/textures/` takes for its cooked `.ktx2` files, one step further:
here the external tool produces the **anchor** as well as the inputs.

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
1ebc5712aafd10b46ea98f30a6410ae57fe074e3e8d8438dd73af7d4bd76fc50  tone-lying-length.ogg
```

The last line is the deliberately corrupt derivative documented below; it is not an ffmpeg
product and has no row in the table above, because ffmpeg is not what produced it.

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

## `tone-lying-length.ogg` — the one fixture whose header LIES, and the only witness for the in-loop cap

`decodeAudioFile` checks each of its three bounds twice: once against the source's own length query,
before anything is reserved, and again inside the read loop. **The second half had no witness
anywhere** until this file existed, because every other fixture here reports an honest length, so the
pre-allocation check always fires first and the loop is never entered with a lie.

This file is `tone.ogg` with its **last page's granule position rewritten from 8000 to 10**, and that
page's framing CRC recomputed so the stream is still structurally valid. Six bytes differ from
`tone.ogg` and nothing else does. It is not an ffmpeg product — no encoder will emit an inconsistent
granule position on purpose — so it is generated by the script below, which is **integer arithmetic
over `tone.ogg`'s own bytes** and therefore reproduces the committed file byte for byte on any
machine with a Python 3:

```bash
# run from THIS directory
python3 - <<'EOF'
import struct
# The ogg framing CRC-32: polynomial 0x04c11db7, no reflection, init 0, no final xor.
table = []
for i in range(256):
    r = i << 24
    for _ in range(8):
        r = ((r << 1) ^ 0x04c11db7) & 0xffffffff if r & 0x80000000 else (r << 1) & 0xffffffff
    table.append(r)
def ogg_crc(b):
    c = 0
    for x in b:
        c = ((c << 8) & 0xffffffff) ^ table[((c >> 24) & 0xff) ^ x]
    return c

src = bytearray(open('tone.ogg', 'rb').read())
# Walk the pages: 'OggS', version, header type, granulepos(8), serial(4), seqno(4), crc(4),
# segment count(1), segment table, body.
offset, pages = 0, []
while offset < len(src):
    assert src[offset:offset + 4] == b'OggS'
    segments = src[offset + 26]
    body = sum(src[offset + 27:offset + 27 + segments])
    pages.append((offset, 27 + segments + body))
    offset += 27 + segments + body
start, size = pages[-1]
src[start + 6:start + 14] = struct.pack('<Q', 10)          # THE LIE: 8000 -> 10
src[start + 22:start + 26] = b'\x00\x00\x00\x00'           # the CRC is computed over a zeroed field
src[start + 22:start + 26] = struct.pack('<I', ogg_crc(bytes(src[start:start + size])))
open('tone-lying-length.ogg', 'wb').write(bytes(src))
EOF
cmp -l tone.ogg tone-lying-length.ogg | wc -l    # -> 6
```

**Why the ogg and not the mp3, measured rather than assumed.** The obvious candidate was an mp3 whose
Xing/Info frame count is a lie — dr_mp3 trusts that field unclamped when it *reports* a length
(`miniaudio.h:94974`). It does not work, in either direction, and both halves were run:

| Seeded claim | What the decoder did |
|---|---|
| `tone.mp3`'s Info count 16 → **4** | decoded **1088** frames, not 8000 |
| `tone.mp3`'s Info count 16 → **100000** | refused by the **pre-allocation** check, naming 57 598 784 frames |

`ma_dr_mp3_read_pcm_frames_raw` **bounds its own reads by the same `totalPCMFrameCount` it reports**
(`miniaudio.h:95221`, the `break` at the padding boundary), so a low lie makes the decode *shorter*
and a high lie is caught before the loop is reached. The same is true of dr_wav: a hand-built Wave64
whose `fact` chunk claims 10 frames over 8000 frames of data decodes **10 frames**. **Within the
miniaudio backend the in-loop check is structurally unreachable.**

stb_vorbis is different, and the difference is visible in its own source: `total_samples` is set
lazily and read by **`stb_vorbis_stream_length_in_samples` alone** — the pull API's decode loop never
consults it. So a low granule position makes the stream *report* 10 frames while
`stb_vorbis_get_samples_short_interleaved` keeps handing back pages. Measured against this file:

| Input | `maxFrames` | Result |
|---|---|---|
| `tone.ogg` | 28 800 000 | 8000 frames |
| `tone.ogg` | 100 | refused: *"the source declares 8000 frames, over the cap of 100"* — **pre-allocation** |
| `tone-lying-length.ogg` | 28 800 000 | **7168 frames** — against the 10 its header claims |
| `tone-lying-length.ogg` | 100 | refused: *"the source decodes to more than 100 frames, the cap this build reads"* — **in-loop** |

7168 rather than 8000 because the *last* page is truncated to its own granule position; every page
before it decodes in full, which is the overrun the in-loop check exists to stop. **The two messages
are what the test asserts**, not the statuses: both inputs are refused at `maxFrames = 100`, and only
the wording says which check did it.

**This file is a corrupt stream on purpose. Never "repair" it, never regenerate it from an encoder,
and never use it as a decode reference for anything.** Its only job is to lie about its length.

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

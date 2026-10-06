# Editor fonts — IBM Plex and Lucide (vendored data)

Font **data** for the editor's user interface. `cmake/embed.cmake` turns each `.ttf` into a generated C++
translation unit at build time; the result is compiled into `aero_editor_core` and nothing else. Nothing under
`engine/` or `runtime/` reads these files, and an exported game never contains them.

## NO LOCAL MODIFICATIONS, EVER

Every file below except this README is **byte-identical** to its upstream, and `.gitattributes` marks both
directories `-text` so git can never rewrite a line ending (IBM Plex's licence ships with CRLF). The fonts are
**never subset**: IBM Plex is licensed under the SIL Open Font License 1.1 with the **Reserved Font Name "Plex"**,
so a subset or otherwise modified copy would be a *Modified Version* that may not carry that name. ImGui bakes
glyphs on demand, so an unused glyph costs file bytes and nothing else.

## IBM Plex — `ibm-plex/`

| File | Upstream | SHA-256 | Bytes |
|---|---|---|---|
| `IBMPlexSans-Regular.ttf` | GitHub `IBM/plex`, release `@ibm/plex-sans@1.1.0`, asset `ibm-plex-sans.zip`, `ibm-plex-sans/fonts/complete/ttf/` | `975dcda37d80f038dcd143c22e33ca2d97a0cc5a929aace1c749153b0fe1afa5` | 200 500 |
| `IBMPlexSans-SemiBold.ttf` | same release and path | `a20caf8286023a6a7a85e40b1d2a4ae9fc3e3b1f9eda8f4c542dd4986af67bb1` | 202 632 |
| `IBMPlexMono-Regular.ttf` | GitHub `IBM/plex`, release `@ibm/plex-mono@2.5.0`, asset `ibm-plex-mono.zip`, `ibm-plex-mono/fonts/complete/ttf/` | `7c6fbddca4b700be918f5f6183d9bd4464fa427fe435f0b480d77fe2bb8c5a43` | 173 052 |
| `OFL.txt` | either release's `LICENSE.txt` (the two are identical) | `7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da` | 4 456 |

Licence: **SIL Open Font License 1.1**, Copyright © 2017 IBM Corp., Reserved Font Name "Plex". The OFL permits
bundling and embedding the fonts in software under any licence, MIT included, provided the fonts are not sold by
themselves and no modified version uses the Reserved Font Name.

## Lucide — `lucide/`

| File | Upstream | SHA-256 | Bytes |
|---|---|---|---|
| `lucide.ttf` | npm `lucide-static@1.49.0`, `package/font/lucide.ttf` | `d76dbb156455cc3ca20b5d387904c921893597dbad5a02728eb407173511ac8e` | 906 976 |
| `codepoints.json` | same package, `package/font/codepoints.json` | `00a7d284de9b6933692a437d0d92666226eda0f00d75600ac4ecc75d619dee3d` | 52 295 |
| `LICENSE` | same package, `package/LICENSE` | `b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57` | 3 208 |

Licence: **ISC**, Copyright (c) 2026 Lucide Icons and Contributors; the icons derived from Feather are **MIT**,
Copyright (c) 2013-present Cole Bemis. Both notices are in the one upstream `LICENSE`. `codepoints.json` maps
icon names to code points; `tests/editor/editor_glyphs_test.cpp` reads it to pin the editor's icon roster.

## Verify

```bash
cd editor/third_party/fonts
shasum -a 256 ibm-plex/*.ttf ibm-plex/OFL.txt lucide/lucide.ttf lucide/codepoints.json lucide/LICENSE
```

Every line must equal the tables above.

## Upgrading

1. Download the new release from the upstream named above and replace the files — never edit one.
2. Update the tables (version, SHA-256, bytes) in the same commit.
3. Rebuild and run `GL4` (every roster name still maps to its code point in `codepoints.json`) and `I277`–`I279`
   (the faces, the icons and their centring, the coverage): a Lucide release that moves a code point reddens
   `GL4`, and one that drops or re-draws a glyph reddens `I278`.
4. Re-run the validation page's visual rows (the type, the icons, nothing clipping).

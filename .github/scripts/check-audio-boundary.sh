#!/usr/bin/env bash
# Aero Engine — the audio-boundary architecture guard (task 3.7.3; docs/04's guard table; epic 3.7's
# Definition of Done: "no miniaudio type is public (guard-enforced)").
#
# THE INVARIANT, IN TWO HALVES:
#
#   1. THE NO-VCPKG PROPERTY. aero_assets, aero_audio and aero_scene_audio link NO vcpkg package at
#      all, which is what makes their PRIVATE links a REAL compile-time boundary rather than
#      convention-plus-grep (R12, docs/08-risks.md: vcpkg installs every port into ONE shared
#      per-triplet include/ root that lands on the compile line of any target linking any vcpkg
#      package). Each CMakeLists says "NO find_package. NOT ONE, EVER." in its own header — and
#      adding one anyway voids the property SILENTLY WHILE CI STAYS GREEN. This guard is what makes
#      that sentence enforceable: the link line makes the boundary green TODAY by construction; a
#      guard is what proves it STAYS green, which is a different job.
#
#   2. THE MINIAUDIO TOKEN BAN. No miniaudio token — <miniaudio.h> or a ma_-prefixed identifier used
#      as code — anywhere under engine/audio/ or engine/scene_audio/, sources INCLUDED. miniaudio's
#      only legal engine home is engine/platform/src/{audio_device.cpp, miniaudio_impl.c} (guarded
#      there by check-platform-boundary.sh + tests/platform_boundary_probe.cpp since 0.3.3); its only
#      other home is editor/src/audio_decode.cpp, on the far side of the boundary rule.
#
# WHY engine/assets IS IN AN "AUDIO-BOUNDARY" SCRIPT — the question the next reader will have. The
# no-vcpkg property is ONE invariant across THREE files, not an audio one: all three carry the same
# "NO find_package. NOT ONE, EVER." prohibition header, the same R12 rationale and the same voiding
# vector, and CLAUDE.md's standing ritual read all three as a single grep. engine/assets additionally
# holds half the audio pipeline (cooked_audio.cpp, audio_cook.cpp). The guard lives with the audio
# epic because 3.7.3 is the task the invariant's own comments hand it to by name; guarding two of the
# three files would have left a known manual ritual half-automated forever.
#
# WHY THE TEXTUAL HALF IS NOT REDUNDANT WITH THE COMPILE ERROR. The hard-compile-error property holds
# only where the compile line is vcpkg-free — and in the *-release presets it is NOT: the three
# targets link aero::profiling PRIVATE, an INTERFACE library that carries Tracy::TracyClient (and
# with it the whole shared vcpkg include root) when AERO_ENABLE_PROFILING=ON. A stray
# `#include <miniaudio.h>` in engine/audio therefore compiles CLEAN in Release and fails only in
# Debug. This guard reddens it on every push, in every configuration, before any compiler runs.
#
# WHY A SCRIPT WITH COMMENT-STRIPPING, NOT A ONE-LINE grep (the platform/rhi/scene precedent): the
# audio headers cite ma_device_uninit / ma_spatializer in first-party `//` prose (audio.hpp,
# system.hpp, spatial.hpp — the lifetime rule and the recorded ma_spatializer deviation), and the
# three CMakeLists each carry the words "find_package" and "#include <miniaudio.h>" inside their own
# prohibition comments. A bare grep is permanently red on a clean tree; CLAUDE.md carried that as a
# "grep that must be READ rather than counted" ritual, and this script is that reading, mechanized:
# `#`/`//` comments are stripped before every identifier/command check.
#
# GENERATOR EXPRESSIONS ARE AN EXIT 1, NOT AN EVASION, AND THAT IS DELIBERATE. 2.1.2 found a genex
# hole in the golden-rule link walk ($<$<CONFIG:Debug>:aero::editor_core> evaded a naive tokeniser),
# so it is worth saying where this guard stands: LINK_TOKEN_RE is an ALLOWLIST anchored at both ends,
# so a genex-shaped token on one of these three link lines matches nothing and is reported as "not an
# aero:: engine target". There is no genex arm because none is needed — the fail-loud direction is
# already the default here. Wanting a genex on one of these lines is a conscious guard edit.
#
# Run it locally before pushing:
#     bash .github/scripts/check-audio-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# The three vcpkg-free CMakeLists — the targets whose PRIVATE links are a real compile-time boundary.
# assets is here alongside the two audio targets because the property is ONE invariant across all
# three (CLAUDE.md's standing find_package grep covered all three as one ritual); the guard lives
# with the audio epic because 3.7.3 is the task the invariant's own comments hand it to.
# A FOURTH VCPKG-FREE TARGET MUST ADD ITSELF HERE IN THE COMMIT THAT CREATES IT: intent cannot be
# derived from the tree, so an unlisted target is silently unguarded.
readonly VCPKG_FREE_CMAKE=('engine/assets/CMakeLists.txt' 'engine/audio/CMakeLists.txt' 'engine/scene_audio/CMakeLists.txt')

# The two audio subsystems the miniaudio token ban governs, sources included (NOT just public
# headers: check-platform-boundary.sh already rejects ma_ in every engine/*/include/* header; what it
# does not cover is an engine/audio .cpp reaching for a device type — the exact shape
# engine/audio/CMakeLists.txt's own header warns 3.7.2 could void it with).
readonly AUDIO_ROOTS=('engine/audio' 'engine/scene_audio')

# CMake commands that can put a vcpkg (or any external) header/library onto these targets' compile
# or link lines. Matched case-insensitively ON COMMENT-STRIPPED TEXT — CMake command names are
# case-insensitive, and the prohibition comments themselves contain "find_package". include_directories
# / link_directories / link_libraries (the directory-scoped forms) and add_subdirectory are banned
# outright: none has a legitimate use in these three files, and a future legitimate need edits this
# guard consciously (the ALLOWED_FILE precedent: widening is a design change, not a chore).
# THE LEADING (^|[^a-zA-Z0-9_]) IS LOAD-BEARING, NOT DECORATION: target_include_directories( and
# target_link_libraries( CONTAIN the banned substrings include_directories( and link_libraries(, so
# without it this guard is permanently red on a clean tree. Self-test 3 pins both directions.
readonly BANNED_CMAKE_RE='(^|[^a-zA-Z0-9_])(find_package|find_path|find_library|find_file|find_program|pkg_check_modules|pkg_search_module|include_directories|link_directories|link_libraries|add_subdirectory)[[:space:]]*\('

# A cross-directory target_link_libraries (CMake >= 3.13) naming one of the three guarded targets --
# the form that voids the property from OUTSIDE the guarded files while all three stay byte-identical.
# THE TRAILING ([^a-zA-Z0-9_]|$) IS LOAD-BEARING TOO: tests/CMakeLists.txt holds
# target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio) -- a line THIS TASK added,
# inside the swept set -- and only that boundary keeps it from matching aero_audio. Self-test 3 pins
# both directions, using this same literal so the self-test cannot drift from the sweep.
readonly CROSSDIR_TLL_RE='target_link_libraries[[:space:]]*\([[:space:]]*aero_(assets|audio|scene_audio)([^a-zA-Z0-9_]|$)'

# A real #include of the miniaudio header (flat: the vcpkg port installs include/miniaudio.h).
readonly MA_INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]miniaudio\.h'
# A real miniaudio identifier used as CODE. Lowercase ma_ is the entire public surface (ma_device,
# ma_decoder, ma_dr_wav, ...); bracket boundary, NOT \b (BSD grep degrades \b to a literal 'b' --
# posix-ere-word-boundary-trap), applied AFTER stripping // comments.
readonly MA_IDENTIFIER_RE='(^|[^a-zA-Z0-9_])ma_'

# Link tokens legal inside the three CMakeLists' target_link_libraries calls: the target itself,
# a visibility keyword, or an aero:: engine target. aero::scene_internal is refused BY NAME even
# though it matches the aero:: shape — it carries EnTT::EnTT INTERFACE by design, and
# engine/scene_audio/CMakeLists.txt's own comment says "NEVER aero::scene_internal".
readonly LINK_TOKEN_RE='^(aero_[a-z_]+|aero::[a-z_]+|PUBLIC|PRIVATE|INTERFACE)$'

cd "$(git rev-parse --show-toplevel)"

# Strip CMake `#` line comments. (Bracket comments #[[...]] are not used in this tree; a banned
# command inside one would FALSE-POSITIVE, which is the fail-loud direction for a textual guard.)
strip_cmake() { sed 's|#.*||'; }

# Flatten a comment-stripped CMake file to one line and emit every <command>(...) call as one line.
# The three files' calls contain no nested parens and no ')' in a string — self-test 3 pins the
# extractor against exactly the multi-line shape they use today.
extract_calls() {  # $1 = case-insensitive command-name ERE fragment, stdin = file text
  strip_cmake | tr '\n' ' ' | grep -oiE "(^|[^a-zA-Z0-9_])$1[[:space:]]*\([^)]*\)" || true
}

# --- Self-test 1: the guarded files and roots must all exist and be tracked. -------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  if ! git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    echo "::error::audio-boundary guard: '$f' is not a tracked file. Was the subsystem moved or" >&2
    echo "         renamed? The guard cannot self-verify, so it is refusing to pass. Update" >&2
    echo "         VCPKG_FREE_CMAKE in $0." >&2
    exit 2
  fi
done
for root in "${AUDIO_ROOTS[@]}"; do
  n=0
  while IFS= read -r -d '' file; do
    case "$file" in
      *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) n=$((n + 1)) ;;
    esac
  done < <(git ls-files -z -- "$root")
  if [ "$n" -eq 0 ]; then
    echo "::error::audio-boundary guard: root '$root' contains no tracked C-family sources -- the" >&2
    echo "         token scan is vacuous there. Was the subsystem moved or renamed? Update" >&2
    echo "         AUDIO_ROOTS in $0." >&2
    exit 2
  fi
done

# --- Self-test 2: the canaries -- the prohibition prose and the real ma_ citations. ------------
# Each guarded CMakeLists carries its own "NO find_package" prohibition comment, so the RAW text
# must contain the word while the STRIPPED text contains no banned call: the comment is the in-tree
# proof that comment-stripping is doing real work, exactly the reading CLAUDE.md's manual ritual
# prescribed. If a prohibition comment is ever deleted, the guard refuses to pass rather than
# passing with its canary gone.
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  if ! grep -qiE 'find_package' "$f"; then
    echo "::error::audio-boundary guard: '$f' no longer contains its find_package prohibition" >&2
    echo "         comment -- the guard's comment-strip canary is gone. Restore the prohibition" >&2
    echo "         header (see engine/audio/CMakeLists.txt) or update this self-test in $0." >&2
    exit 2
  fi
done
# The audio headers cite ma_ identifiers in `//` prose (audio.hpp / system.hpp / spatial.hpp -- the
# lifetime rule and the ma_spatializer deviation note). At least one RAW citation must exist across
# the scan set, or the identifier regex has rotted / the scan no longer walks the real files.
raw_ma=0
while IFS= read -r -d '' file; do
  case "$file" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) ;;
    *) continue ;;
  esac
  if grep -qE "$MA_IDENTIFIER_RE" "$file"; then raw_ma=$((raw_ma + 1)); fi
done < <(git ls-files -z -- "${AUDIO_ROOTS[@]}")
if [ "$raw_ma" -eq 0 ]; then
  echo "::error::audio-boundary guard: no raw ma_ citation found anywhere under the audio roots --" >&2
  echo "         the identifier regex or the scan set has rotted (audio.hpp/system.hpp/spatial.hpp" >&2
  echo "         each cite ma_ names in // prose). Fix MA_IDENTIFIER_RE or AUDIO_ROOTS in $0." >&2
  exit 2
fi

# --- Self-test 3: the machinery fires, and does NOT over-match prose. --------------------------
if ! printf 'find_package(miniaudio CONFIG REQUIRED)\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 no longer matches a real find_package call -- vacuous." >&2
  exit 2
fi
if ! printf 'FIND_PACKAGE(Tracy CONFIG REQUIRED)\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 misses an upper-case CMake command -- vacuous (CMake command" >&2
  echo "         names are case-insensitive)." >&2
  exit 2
fi
if printf '# NO find_package. NOT ONE, EVER.\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi
# The two arms below pin the LEADING bracket boundary of BANNED_CMAKE_RE. target_include_directories(
# and target_link_libraries( CONTAIN the banned substrings include_directories( and link_libraries(,
# and every one of the three guarded files uses both commands: drop the boundary and this guard is
# permanently red on a clean tree. Pinned here rather than left to be rediscovered.
if printf 'target_include_directories(aero_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 over-matches target_include_directories -- the leading" >&2
  echo "         (^|[^a-zA-Z0-9_]) boundary has been lost. It is load-bearing, not decoration." >&2
  exit 2
fi
if printf 'target_link_libraries(aero_audio PUBLIC aero::core)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 over-matches target_link_libraries -- same lost boundary." >&2
  exit 2
fi
if ! printf 'target_link_libraries(aero_audio\n    PUBLIC aero::core aero::assets\n    PRIVATE miniaudio\n)\n' \
    | extract_calls 'target_link_libraries' | grep -qE '(^|[^a-zA-Z0-9_])miniaudio($|[^a-zA-Z0-9_])'; then
  echo "::error::The target_link_libraries extractor in $0 no longer sees a multi-line call --" >&2
  echo "         vacuous. Fix extract_calls." >&2
  exit 2
fi
# The two arms below pin CROSSDIR_TLL_RE's TRAILING name boundary, against the same literal Part 1d
# sweeps with. tests/CMakeLists.txt -- which is inside the swept set -- holds
# target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio), the tree's first near-miss
# for this regex, and only that boundary separates it from a real cross-directory link.
if ! printf 'target_link_libraries(aero_audio PRIVATE miniaudio)\n' | grep -qiE "$CROSSDIR_TLL_RE"; then
  echo "::error::CROSSDIR_TLL_RE in $0 no longer matches a real cross-directory link -- vacuous." >&2
  exit 2
fi
if printf 'target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio)\n' | grep -qiE "$CROSSDIR_TLL_RE"; then
  echo "::error::CROSSDIR_TLL_RE in $0 matches aero_audio_boundary_probe -- the trailing name" >&2
  echo "         boundary has been lost, and this guard would red the tree it ships into." >&2
  exit 2
fi
if ! printf '#include <miniaudio.h>\n' | grep -qE "$MA_INCLUDE_RE"; then
  echo "::error::MA_INCLUDE_RE in $0 no longer matches a real miniaudio include -- vacuous." >&2
  exit 2
fi
if ! printf 'ma_device* leak;\n' | grep -qE "$MA_IDENTIFIER_RE"; then
  echo "::error::MA_IDENTIFIER_RE in $0 no longer matches a real miniaudio identifier -- vacuous." >&2
  exit 2
fi
if printf '// wraps a ma_device\n' | sed 's|//.*||' | grep -qE "$MA_IDENTIFIER_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure // comment line still matches." >&2
  exit 2
fi

violations=""

# --- Part 1a: no vcpkg hook command in the three CMakeLists. -----------------------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  hits="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -iE "$BANNED_CMAKE_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${f}:${n}: a vcpkg/dependency hook command entered a vcpkg-free CMakeLists
"
    done <<< "$hits"
  fi
done

# --- Part 1b: every link token in the three CMakeLists is an aero:: engine target. -------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  calls="$(extract_calls 'target_link_libraries' < "$f")"
  [ -z "$calls" ] && continue
  while IFS= read -r call; do
    tokens="$(printf '%s\n' "$call" | sed -E 's/^[^(]*\(//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d')"
    while IFS= read -r tok; do
      if [ "$tok" = "aero::scene_internal" ]; then
        line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F 'scene_internal' | head -1 | cut -d: -f1 || true)"
        violations="${violations}${f}:${line:-1}: aero::scene_internal on a link line -- it carries EnTT::EnTT INTERFACE by design
"
      elif ! printf '%s\n' "$tok" | grep -qE "$LINK_TOKEN_RE"; then
        line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F -- "$tok" | head -1 | cut -d: -f1 || true)"
        violations="${violations}${f}:${line:-1}: link token '${tok}' is not an aero:: engine target -- this CMakeLists is vcpkg-free by contract
"
      fi
    done <<< "$tokens"
  done <<< "$calls"
done

# --- Part 1c: include directories stay inside the subsystem. -----------------------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  calls="$(extract_calls 'target_include_directories' < "$f")"
  [ -z "$calls" ] && continue
  while IFS= read -r call; do
    tokens="$(printf '%s\n' "$call" | sed -E 's/^[^(]*\(//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d')"
    while IFS= read -r tok; do
      case "$tok" in
        aero_*|PUBLIC|PRIVATE|INTERFACE) ;;
        '${CMAKE_CURRENT_SOURCE_DIR}'*) ;;
        *)
          line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F -- "$tok" | head -1 | cut -d: -f1 || true)"
          violations="${violations}${f}:${line:-1}: include dir '${tok}' reaches outside the subsystem -- only \${CMAKE_CURRENT_SOURCE_DIR}-rooted paths are legal here
"
          ;;
      esac
    done <<< "$tokens"
  done <<< "$calls"
done

# --- Part 1d: no OTHER CMake file mutates the three targets' link lines (CMake >= 3.13 allows a
# cross-directory target_link_libraries, which would void the property from outside the guarded
# files). ---------------------------------------------------------------------------------------
swept=0
while IFS= read -r -d '' f; do
  case "$f" in
    engine/assets/CMakeLists.txt|engine/audio/CMakeLists.txt|engine/scene_audio/CMakeLists.txt) continue ;;
  esac
  swept=$((swept + 1))
  hits="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -iE "$CROSSDIR_TLL_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${f}:${n}: cross-directory target_link_libraries on a vcpkg-free target
"
    done <<< "$hits"
  fi
done < <(git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake')

# --- Part 2: no miniaudio token anywhere under the audio roots (sources included). -------------
scanned=0
while IFS= read -r -d '' file; do
  case "$file" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) ;;
    *) continue ;;
  esac
  scanned=$((scanned + 1))
  stripped="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$MA_IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: miniaudio identifier used as code inside the audio layer
"
    done <<< "$hits"
  fi
  inc_hits="$(grep -nE "$MA_INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: miniaudio header included inside the audio layer
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "${AUDIO_ROOTS[@]}")

if [ -n "$violations" ]; then
  echo "audio boundary broken -- task 3.7.3 / ADR-006 / docs/04 guard table / epic 3.7 DoD:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "engine/audio and engine/scene_audio are first-party layers over engine types ONLY, and" >&2
  echo "aero_assets/aero_audio/aero_scene_audio link no vcpkg package -- that is what makes their" >&2
  echo "PRIVATE links a real compile-time boundary (R12). A device belongs behind" >&2
  echo "platform::AudioDevice (engine/platform); a decoder belongs in editor/src/audio_decode.cpp." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::audio boundary broken (task 3.7.3; ADR-006). Keep engine/audio + engine/scene_audio miniaudio-free and their CMakeLists vcpkg-free; route devices through platform::AudioDevice and decodes through editor/src/audio_decode.cpp."
    done <<< "$violations"
  fi
  exit 1
fi

echo "audio-boundary guard: OK -- ${scanned} tracked engine/audio+engine/scene_audio sources scanned; 3 vcpkg-free CMakeLists verified; ${swept} other CMake files swept for cross-directory links; miniaudio confined to engine/platform/src/{audio_device.cpp, miniaudio_impl.c} + editor/src/audio_decode.cpp"

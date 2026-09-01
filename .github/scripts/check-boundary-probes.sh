#!/usr/bin/env bash
# Aero Engine — the boundary-probe integrity guard (task 3.7.3, taking the handoff 0.2.3 opened and
# 0.4.5 §7 renewed by name: "a vcpkg package silently joining a probe's link line" is the ONE way
# every compile-time boundary probe can rot, and it rots WHILE CI STAYS GREEN).
#
# THE INVARIANT: every aero_*_boundary_probe target in tests/CMakeLists.txt links EXACTLY ONE
# aero:: library, PRIVATE, and nothing else. Each probe's whole guarantee is that vcpkg's shared
# per-triplet include/ root is NOT on its compile line (R12, docs/08-risks.md), so the third-party
# backend behind its library is genuinely unresolvable there. Adding doctest::doctest,
# aero::profiling (Tracy, a vcpkg package, in Release), the backend itself, or any aero::*_internal
# target (scene_internal carries EnTT::EnTT INTERFACE by design) restores the shared root and
# silently reduces that probe to a no-op. Every probe block says "DO NOT ADD ANY OTHER LINK
# LIBRARY" in prose; this guard is the sentence made mechanical.
#
# THE PROBE LIST IS DERIVED FROM THE REGISTRY, NEVER ENUMERATED HERE: a future probe (script/quickjs
# is R12's named next case) is covered the moment its add_library(... OBJECT ...) lands, with no
# edit to this file. The one hardcoded name below is a CANARY, not a roster: the audio probe this
# guard shipped beside must be in the derived set, or derivation itself has rotted.
#
# Run it locally before pushing:
#     bash .github/scripts/check-boundary-probes.sh
# NOTE: it reads the TRACKED registry path from the repo root; a probe added to a working copy is
# seen as soon as the file is saved, since the file itself is read rather than listed by git.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

readonly PROBE_REGISTRY='tests/CMakeLists.txt'
readonly CANARY_PROBE='aero_audio_boundary_probe'
# EXACTLY one library token, and it must be a plain aero:: engine target. *_internal is refused by
# the second pattern even though it matches the first: those targets carry their backend INTERFACE
# by design (check-scene-boundary.sh's own warning). No digits: a future aero::box2d probe is
# reported as a non-engine token rather than waved through -- fail-loud, and a conscious
# one-character edit when it happens.
readonly PROBE_LIB_RE='^aero::[a-z_]+$'

cd "$(git rev-parse --show-toplevel)"

strip_cmake() { sed 's|#.*||'; }
flat() { strip_cmake < "$PROBE_REGISTRY" | tr '\n' ' '; }

# Every add_library(<name> OBJECT ...) whose <name> ends in _boundary_probe -> the derived set.
probes="$(flat | grep -oiE '(^|[^a-zA-Z0-9_])add_library[[:space:]]*\([[:space:]]*[a-zA-Z0-9_]+[[:space:]]+OBJECT' \
          | sed -E 's/.*\([[:space:]]*//; s/[[:space:]]+OBJECT.*//' | grep -E '_boundary_probe$' | sort -u || true)"

# --- Self-test 1: derivation found something, and found the canary. ----------------------------
if [ -z "$probes" ]; then
  echo "::error::boundary-probe guard derived an EMPTY probe set from ${PROBE_REGISTRY} -- the" >&2
  echo "         extraction has rotted (or every probe was deleted). Fix this script or the registry." >&2
  exit 2
fi
if ! printf '%s\n' "$probes" | grep -qx "$CANARY_PROBE"; then
  echo "::error::boundary-probe guard: '${CANARY_PROBE}' is not in the derived probe set. Renamed?" >&2
  echo "         The guard cannot self-verify, so it is refusing to pass. Update CANARY_PROBE in $0." >&2
  exit 2
fi

# --- Self-test 2: the validator fires on synthetic contamination. ------------------------------
check_tokens() {  # $1 = probe name, $2 = whitespace-separated tokens after the target name
  libs=0
  for tok in $2; do
    case "$tok" in
      PRIVATE) ;;
      PUBLIC|INTERFACE)
        printf '%s\n' "visibility '$tok' (must be PRIVATE: an OBJECT probe propagates nothing, and PUBLIC invites reuse that contaminates)"; return ;;
      *_internal|aero::*_internal)
        printf '%s\n' "internal target '$tok' (carries its backend INTERFACE by design)"; return ;;
      *)
        if printf '%s\n' "$tok" | grep -qE "$PROBE_LIB_RE"; then libs=$((libs + 1)); else
          printf '%s\n' "non-engine token '$tok'"; return
        fi ;;
    esac
  done
  if [ "$libs" -ne 1 ]; then printf '%s\n' "$libs aero:: libraries (must be exactly 1)"; fi
}
if [ -z "$(check_tokens t 'PRIVATE aero::rhi doctest::doctest')" ]; then
  echo "::error::check_tokens in $0 no longer flags a vcpkg package on a probe link line -- vacuous." >&2
  exit 2
fi
if [ -z "$(check_tokens t 'PRIVATE aero::scene_internal')" ]; then
  echo "::error::check_tokens in $0 no longer flags an *_internal target -- vacuous." >&2
  exit 2
fi
if [ -n "$(check_tokens t 'PRIVATE aero::audio')" ]; then
  echo "::error::check_tokens in $0 flags the canonical single-library shape -- over-broad." >&2
  exit 2
fi

# --- The guard. --------------------------------------------------------------------------------
violations=""
count=0
for probe in $probes; do
  count=$((count + 1))
  calls="$(flat | grep -oiE "(^|[^a-zA-Z0-9_])target_link_libraries[[:space:]]*\([[:space:]]*${probe}([^a-zA-Z0-9_][^)]*)?\)" || true)"
  ncalls="$(printf '%s' "$calls" | grep -c 'target_link_libraries' || true)"
  if [ "$ncalls" -eq 0 ]; then
    violations="${violations}${PROBE_REGISTRY}: ${probe} has NO target_link_libraries call -- it compiles nothing aero and asserts nothing
"
    continue
  fi
  if [ "$ncalls" -ne 1 ]; then
    violations="${violations}${PROBE_REGISTRY}: ${probe} has ${ncalls} target_link_libraries calls -- a second call APPENDS libraries and is how a probe rots
"
    continue
  fi
  tokens="$(printf '%s\n' "$calls" | sed -E 's/^[^(]*\([[:space:]]*//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d' | sed '1d' | tr '\n' ' ')"
  problem="$(check_tokens "$probe" "$tokens")"
  if [ -n "$problem" ]; then
    line="$(nl -ba -w1 -s: "$PROBE_REGISTRY" | sed 's|#.*||' | grep -E "target_link_libraries[[:space:]]*\([[:space:]]*${probe}([^a-zA-Z0-9_]|\))" | head -1 | cut -d: -f1 || true)"
    violations="${violations}${PROBE_REGISTRY}:${line:-1}: ${probe} links ${problem}
"
  fi
done

if [ -n "$violations" ]; then
  echo "a boundary probe's link line rotted -- task 3.7.3 / R12 (docs/08-risks.md):" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "A probe target must link EXACTLY ONE aero:: library, PRIVATE. Anything else puts vcpkg's" >&2
  echo "shared include root back on its compile line and silently voids the compile-time boundary" >&2
  echo "while CI stays green -- the one failure mode these probes cannot survive." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      case "$n" in (*[!0-9]*) n=1 ;; esac
      echo "::error file=${f},line=${n}::a boundary probe's link line rotted (task 3.7.3; R12). A probe links exactly one aero:: library, PRIVATE -- nothing else, ever."
    done <<< "$violations"
  fi
  exit 1
fi

echo "boundary-probe guard: OK -- ${count} probe targets verified in ${PROBE_REGISTRY}, each linking exactly one aero:: library PRIVATE"

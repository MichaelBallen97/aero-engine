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
# NOTE ON WHAT IS AND IS NOT TRACKED-ONLY, because this guard is now half of each. The REGISTRY is
# read as a file, so a probe added to a working copy is seen as soon as the file is saved. The
# CROSS-FILE SWEEP walks `git ls-files`, so it sees TRACKED files only -- `git add` a new CMake file
# before expecting an append hidden in it to be caught.
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
flat_file() { strip_cmake < "$1" | tr '\n' ' '; }

# Every target_link_libraries(...) call in a flattened CMake file, one per line.
tll_calls() {  # $1 = path
  flat_file "$1" | grep -oiE "(^|[^a-zA-Z0-9_])target_link_libraries[[:space:]]*\([^)]*\)" || true
}

# The TARGET token of one extracted call -- its first argument. `sed -n '1p'` rather than `head -1`:
# head closes the pipe early and the SIGPIPE would fail the command substitution under pipefail.
tll_target() {  # stdin = one extracted call
  sed -E 's/^[^(]*\([[:space:]]*//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d' | sed -n '1p'
}

# Every set_target_properties(...) / set_property(TARGET ...) call in a flattened CMake file.
# PARENTHESISED alternation: interpolated bare, `a|b` would bind as `(^|[^..])a` OR `b...\)`, which
# matches the first command's NAME with no call body and silently drops every wrapped call.
prop_calls() {  # $1 = path
  flat_file "$1" | grep -oiE "(^|[^a-zA-Z0-9_])(set_target_properties|set_property)[[:space:]]*\([^)]*\)" || true
}

# The TARGET tokens of one such call, one per line -- both spellings, since both reach the same
# properties:
#   set_target_properties(<t...> PROPERTIES <prop> <val> ...)
#   set_property(TARGET <t...> [APPEND|APPEND_STRING] PROPERTY <prop> <val> ...)
# Any other set_property scope names no target and prints nothing.
prop_targets() {  # $1 = one extracted call
  _body="$(printf '%s\n' "$1" | sed -E 's/^[^(]*\([[:space:]]*//; s/\)$//')"
  for _tok in $_body; do
    case "$_tok" in
      TARGET) continue ;;
      PROPERTIES|PROPERTY) return 0 ;;
      APPEND|APPEND_STRING) continue ;;
      GLOBAL|DIRECTORY|SOURCE|INSTALL|TEST|CACHE) return 0 ;;
    esac
    printf '%s\n' "$_tok"
  done
}

is_probe() { printf '%s\n' "$probes" | grep -qx "$1"; }

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
  privs=0
  for tok in $2; do
    case "$tok" in
      PRIVATE) privs=$((privs + 1)) ;;
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
  if [ "$libs" -ne 1 ]; then printf '%s\n' "$libs aero:: libraries (must be exactly 1)"; return; fi
  # PRIVATE must be PRESENT, not merely un-contradicted. Rejecting PUBLIC/INTERFACE is not the same
  # check: the PLAIN signature -- target_link_libraries(<target> <lib>) with no keyword at all -- is
  # CMake's transitive/all-keyword form, and it passed while the banner claimed "each linking exactly
  # one aero:: library PRIVATE" (measured, 3.7.3's code-review round). Claiming enforcement that is
  # not delivered is the one thing .claude/rules/boundary-guards.md names outright.
  if [ "$privs" -ne 1 ]; then
    printf '%s\n' "$privs PRIVATE keywords (must be exactly 1: the plain target_link_libraries(<target> <lib>) signature is transitive)"
  fi
}
if [ -z "$(check_tokens t 'PRIVATE aero::rhi doctest::doctest')" ]; then
  echo "::error::check_tokens in $0 no longer flags a vcpkg package on a probe link line -- vacuous." >&2
  exit 2
fi
if [ -z "$(check_tokens t 'PRIVATE aero::scene_internal')" ]; then
  echo "::error::check_tokens in $0 no longer flags an *_internal target -- vacuous." >&2
  exit 2
fi
if [ -z "$(check_tokens t 'aero::audio')" ]; then
  echo "::error::check_tokens in $0 no longer flags a link line with NO PRIVATE keyword -- the plain" >&2
  echo "         signature is transitive, and this guard's own banner claims PRIVATE." >&2
  exit 2
fi
if [ -n "$(check_tokens t 'PRIVATE aero::audio')" ]; then
  echo "::error::check_tokens in $0 flags the canonical single-library shape -- over-broad." >&2
  exit 2
fi

# --- Self-test 2b: the property machinery. Both spellings reach EXCLUDE_FROM_ALL and LINK_LIBRARIES,
# so both must be read; a non-TARGET scope must name nothing; and the extractor must survive wrapping.
if [ "$(prop_targets 'set_target_properties(aero_audio_boundary_probe PROPERTIES EXCLUDE_FROM_ALL TRUE)')" \
     != 'aero_audio_boundary_probe' ]; then
  echo "::error::prop_targets in $0 no longer reads a set_target_properties target -- the property" >&2
  echo "         spelling of EXCLUDE_FROM_ALL would pass while the banner claims 'built by all'." >&2
  exit 2
fi
if [ "$(prop_targets 'set_property(TARGET aero_audio_boundary_probe APPEND PROPERTY LINK_LIBRARIES doctest::doctest)')" \
     != 'aero_audio_boundary_probe' ]; then
  echo "::error::prop_targets in $0 no longer reads a set_property(TARGET ...) target -- vacuous." >&2
  exit 2
fi
if [ -n "$(prop_targets 'set_property(GLOBAL PROPERTY USE_FOLDERS ON)')" ]; then
  echo "::error::prop_targets in $0 names a target for a non-TARGET scope -- over-broad." >&2
  exit 2
fi

# --- The guard. --------------------------------------------------------------------------------
violations=""
count=0
for probe in $probes; do
  count=$((count + 1))
  # EXCLUDE_FROM_ALL is a link line that never runs: the target is not built by `all`, so it compiles
  # nothing and asserts nothing in ANY configuration, and every other check below would still pass.
  # Plan §B.3's P-a names this rot mode ("the 0.2.3 silent-green lesson") and it had been verified
  # exactly once, by hand, at implementation time -- nothing noticed afterwards. It is derivable from
  # the same add_library line the probe set comes from, so refusing it costs one grep.
  decl="$(flat | grep -oiE "(^|[^a-zA-Z0-9_])add_library[[:space:]]*\([[:space:]]*${probe}[[:space:]][^)]*\)" || true)"
  if printf '%s\n' "$decl" | grep -qE '(^|[^a-zA-Z0-9_])EXCLUDE_FROM_ALL([^a-zA-Z0-9_]|$)'; then
    violations="${violations}${PROBE_REGISTRY}: ${probe} is declared EXCLUDE_FROM_ALL -- it is never built by \`all\`, so it compiles nothing and asserts nothing in any configuration
"
    continue
  fi
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

# --- The cross-file sweep. ----------------------------------------------------------------------
# CMake >= 3.13 lets ANY CMakeLists append to a target defined elsewhere, so reading only the
# registry left a probe's link line mutable from every other CMake file in the tree -- and CMake's
# target_link_libraries calls ACCUMULATE, so the appended library joins the compile line without the
# registry changing by a byte. check-audio-boundary.sh's Part 1d exists for exactly this capability;
# this is the same sweep, scoped to the derived probe set. Without it, R-b's "exactly one aero::
# library" claim was true of the registry rather than of the probe.
swept=0
while IFS= read -r -d '' f; do
  # A file can be TRACKED and ABSENT (`git add` then `rm` -- the ordinary middle of a rename).
  # Skip it rather than let a read failure take the guard down; the sibling had this crash.
  [ -f "$f" ] || continue
  if [ "$f" != "$PROBE_REGISTRY" ]; then
    swept=$((swept + 1))
    calls="$(tll_calls "$f")"
    if [ -n "$calls" ]; then
      while IFS= read -r call; do
        [ -z "$call" ] && continue
        tgt="$(printf '%s\n' "$call" | tll_target)"
        if is_probe "$tgt"; then
          line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' \
                  | grep -E "(^|[^a-zA-Z0-9_])${tgt}([^a-zA-Z0-9_]|$)" | head -1 | cut -d: -f1 || true)"
          violations="${violations}${f}:${line:-1}: a cross-directory target_link_libraries appends to ${tgt}'s link line from outside ${PROBE_REGISTRY}
"
        fi
      done <<< "$calls"
    fi
  fi
  # The PROPERTY spellings, swept over EVERY tracked CMake file INCLUDING the registry itself.
  # Every predicate this guard enforces has one: LINK_LIBRARIES is what target_link_libraries
  # writes, and EXCLUDE_FROM_ALL is settable long after the add_library that the arm above reads --
  # `set_target_properties(<probe> PROPERTIES EXCLUDE_FROM_ALL TRUE)` in the registry passed with the
  # banner still claiming "each built by `all`" (measured, 3.7.3's second code-review round). A probe
  # has no legitimate use for a target property, so the CALLS are refused rather than one property
  # at a time -- the general form of "match the predicate, not the spelling in front of you".
  props="$(prop_calls "$f")"
  [ -z "$props" ] && continue
  while IFS= read -r call; do
    [ -z "$call" ] && continue
    while IFS= read -r tgt; do
      [ -z "$tgt" ] && continue
      if is_probe "$tgt"; then
        line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' \
                | grep -E "(^|[^a-zA-Z0-9_])${tgt}([^a-zA-Z0-9_]|$)" | head -1 | cut -d: -f1 || true)"
        violations="${violations}${f}:${line:-1}: a target property is set on ${tgt} -- a probe's properties (EXCLUDE_FROM_ALL, LINK_LIBRARIES, INCLUDE_DIRECTORIES) are exactly what its guarantee rests on
"
      fi
    done <<< "$(prop_targets "$call")"
  done <<< "$props"
done < <(git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake')

# Anti-vacuity: a sweep that walked nothing proves nothing and would print the OK banner anyway.
if [ "$swept" -eq 0 ]; then
  echo "::error::boundary-probe guard: the cross-file sweep walked ZERO other CMake files -- its" >&2
  echo "         pathspec has rotted, so nothing proves a probe's link line is unmutated elsewhere." >&2
  exit 2
fi

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

echo "boundary-probe guard: OK -- ${count} probe targets verified in ${PROBE_REGISTRY}, each built by \`all\` (no EXCLUDE_FROM_ALL in either spelling) and linking exactly one aero:: library PRIVATE; ${swept} other CMake files swept for cross-directory appends and target-property writes"

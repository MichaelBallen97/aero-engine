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
#
# BRACKET COMMENTS ARE NOT STRIPPED, and that is a deliberate fail-loud choice both guards make
# identically. `strip_cmake` removes `#` line comments only, so the continuation lines of a
# `#[[ ... ]]` block comment are read as code and a guarded name or banned command inside one is
# reported. Neither style is used anywhere in this tree. The alternative -- parsing bracket comments
# -- would mean a real violation could be hidden inside one, which is the wrong direction for a
# guard; a false positive is a two-line edit, a false negative is another review round.
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

tll_target() {  # stdin = one extracted call
  sed -E 's/^[^(]*\([[:space:]]*//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d' | sed -n '1p'
}


# THE DIRECTORY-SCOPE SET: every file that can push state into the directory scope the probes are
# defined in. Directory properties are inherited, so include_directories() in any of these reaches
# every probe's compile line without naming one -- the direction the naming allowlist cannot see.
#
# Computed, not listed, and INCLUSIVE OF THE REGISTRY'S OWN DIRECTORY. The first version computed
# STRICT ancestors, so tests/CMakeLists.txt itself was never checked -- and it is the likeliest file
# of all for someone to write a directory-scoped command in, because it is where the probes live:
# `link_libraries(doctest::doctest)` prepended to it put doctest and vcpkg's shared include root on
# all six probes' compile lines while the banner read "6 probe targets verified" (measured, 3.7.3's
# fourth review round). The closure also follows include(), because the root includes five
# cmake/*.cmake modules before add_subdirectory(tests) and a directory-scoped command in any of them
# is inherited just the same. It follows REAL include() edges rather than taking every .cmake in the
# tree: an over-approximation would redden this guard's own e2e drivers.
REGISTRY_DIR="${PROBE_REGISTRY%/CMakeLists.txt}"
readonly REGISTRY_DIR

dir_scope_files() {
  _d="$REGISTRY_DIR"
  _seed="${PROBE_REGISTRY}
"
  while case "$_d" in */*) true ;; *) false ;; esac; do
    _d="${_d%/*}"
    _seed="${_seed}${_d}/CMakeLists.txt
"
  done
  _seed="${_seed}CMakeLists.txt
"
  printf '%s' "$_seed" | sed '/^$/d' | sort -u | expand_includes
}

# The transitive closure of a file set under include(). Paths are resolved by stripping the usual
# ${...}/ prefixes and trying repo-relative first, then relative to the including file.
expand_includes() {  # stdin = newline-separated file list
  _cur="$(cat)"
  while : ; do
    _add=''
    while IFS= read -r _f; do
      [ -z "$_f" ] && continue
      [ -f "$_f" ] || continue
      _base="${_f%/*}"
      [ "$_base" = "$_f" ] && _base='.'
      _incs="$(strip_cmake < "$_f" | tr '\n' ' ' \
               | grep -oiE '(^|[^a-zA-Z0-9_])include[[:space:]]*\([^)]*\)' \
               | sed -E 's/^[^(]*\([[:space:]]*//; s/\).*$//' | awk '{print $1}' || true)"
      for _i in $_incs; do
        case "$_i" in
          '${'*'}/'*) _i="${_i#*\}/}" ;;
        esac
        case "$_i" in *.cmake) ;; *) continue ;; esac
        _c="$_i"
        [ -f "$_c" ] || _c="${_base}/${_i}"
        _c="${_c#./}"
        [ -f "$_c" ] || continue
        printf '%s\n' "$_cur" | grep -qxF "$_c" && continue
        printf '%s\n' "$_add" | grep -qxF "$_c" && continue
        _add="${_add}${_c}
"
      done
    done <<< "$_cur"
    [ -z "$_add" ] && break
    _cur="$(printf '%s\n%s' "$_cur" "$_add" | sed '/^$/d' | sort -u)"
  done
  printf '%s\n' "$_cur"
}

# The command name of one extracted call, lower-cased (CMake command names are case-insensitive).
call_name() {  # stdin = one call
  sed -E 's/^[^a-zA-Z_]*//; s/[[:space:]]*\(.*$//' | tr 'A-Z' 'a-z'
}

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

# NOTE ON A SELF-TEST THAT IS GONE, because a removed check should say so rather than vanish.
# Self-test 2b pinned a prop_targets() helper that read the target out of set_target_properties /
# set_property calls. Both are gone: the sweep no longer asks WHICH command names a probe, only
# whether anything other than the two legal calls does, so there is no per-command extractor left to
# rot. The property spellings are covered by stages P15/P16 and by the allowlist itself.

# --- The guard. --------------------------------------------------------------------------------
violations=""
DIR_SCOPE_FILES="$(dir_scope_files)"
readonly DIR_SCOPE_FILES
# Anti-vacuity: an empty or registry-less scope set would silently disable the whole directory-scope
# half while everything else still passed. It caught exactly that during this round's own bring-up.
if ! printf '%s\n' "$DIR_SCOPE_FILES" | grep -qxF "$PROBE_REGISTRY"; then
  echo "::error::boundary-probe guard: the directory-scope set does not contain ${PROBE_REGISTRY}" >&2
  echo "         itself, so nothing checks the file the probes live in. Fix dir_scope_files in $0." >&2
  exit 2
fi
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

# --- The cross-file sweep, INVERTED TO AN ALLOWLIST. ---------------------------------------------
# A probe may legitimately be named by exactly TWO commands: its own `add_library(<probe> OBJECT ...)`
# and its one `target_link_libraries(<probe> PRIVATE aero::x)`. ANY OTHER COMMAND, IN ANY TRACKED
# CMAKE FILE, THAT NAMES A DERIVED PROBE IS A VIOLATION.
#
# That sentence is the whole check, and it replaces three review rounds of enumeration. Each round
# closed the spellings it had been shown and left the class open: an alias but not a raw name, an
# add_library keyword but not a property write, a property write but not the plain command --
# `target_include_directories(<probe> PRIVATE /opt/vcpkg/.../include)` and
# `target_compile_options(<probe> PRIVATE -I...)` both restored vcpkg's shared include root to the
# probe's compile line while this guard printed OK. CMake has too many ways to reach a target for a
# denylist to converge, and the allowlist is bounded, complete, and needs no maintenance: a spelling
# nobody has thought of is a violation on arrival because it is not one of the two.
# Every <command>(...) call in a comment-stripped, flattened file, one per line.
all_calls_file() {  # $1 = path
  flat_file "$1" \
    | grep -oiE '(^|[^a-zA-Z0-9_])[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^)]*\)' || true
}

probe_alt="$(printf '%s\n' "$probes" | tr '\n' '|' | sed 's/|$//')"

swept=0
while IFS= read -r -d '' f; do
  # A file can be TRACKED and ABSENT (`git add` then `rm` -- the ordinary middle of a rename).
  [ -f "$f" ] || continue
  [ "$f" != "$PROBE_REGISTRY" ] && swept=$((swept + 1))
  numbered="$(nl -ba -w1 -s: "$f" | sed 's|#.*||')"
  # A FILE IN THE DIRECTORY-SCOPE SET reaches every probe without naming one: directory properties
  # are inherited, so include_directories() there puts a root on every probe's compile line, and
  # add_subdirectory(<registry dir> EXCLUDE_FROM_ALL) takes every probe out of `all`.
  #
  # READ FROM THE FLATTENED FILE, like every other arm here. The first version grepped numbered
  # lines, so all three keyword checks were line-scoped in a script where everything else flattens --
  # `set_property(DIRECTORY .\n  PROPERTY EXCLUDE_FROM_ALL TRUE)` and both siblings passed wrapped
  # and failed single-line. That is .claude/rules/boundary-guards.md's lesson 3, missed inside the
  # delta that wrote it.
  #
  # THIS ARM IS A DENYLIST AND CANNOT BE COMPLETE -- add_compile_options(-I...), add_definitions and
  # a CMAKE_CXX_FLAGS mutation all reach the same compile line and are not listed. Enumerating them
  # is the game rounds 1-4 lost. What closes the class is the ctest case
  # boundary-probes.probe_compile_line, which reads compile_commands.json and asserts the property
  # directly. This arm stays because it names the vector at push time, before any build runs.
  if printf '%s\n' "$DIR_SCOPE_FILES" | grep -qxF "$f"; then
    flat="$(flat_file "$f")"
    dhit="$(printf '%s\n' "$flat" \
            | grep -oiE "(^|[^a-zA-Z0-9_])(include_directories|link_directories|link_libraries)[[:space:]]*\(" \
            | head -1 || true)"
    ehit="$(printf '%s\n' "$flat" \
            | grep -oiE "(^|[^a-zA-Z0-9_])(set_property[[:space:]]*\([[:space:]]*DIRECTORY|set_directory_properties[[:space:]]*\(|add_subdirectory[[:space:]]*\([^)]*${REGISTRY_DIR}[^)]*)[^)]*EXCLUDE_FROM_ALL" \
            | head -1 || true)"
    for _hv in "$dhit" "$ehit"; do
      [ -z "$_hv" ] && continue
      _cmd="$(printf '%s\n' "$_hv" | sed -E 's/^[^a-zA-Z_]*//; s/[[:space:]]*\(.*$//' | tr 'A-Z' 'a-z')"
      line="$(printf '%s\n' "$numbered" | grep -iE "(^|[^a-zA-Z0-9_])${_cmd}[[:space:]]*\(" \
              | head -1 | cut -d: -f1 || true)"
      violations="${violations}${f}:${line:-1}: '${_cmd}' here reaches every probe in ${PROBE_REGISTRY} without naming one -- directory properties are inherited
"
    done
  fi
  # Cheap reject: the overwhelming majority of CMake files never mention a probe, and only the ones
  # that do pay for call extraction.
  printf '%s\n' "$numbered" | grep -qE "(^|[^a-zA-Z0-9_])(${probe_alt})([^a-zA-Z0-9_]|$)" || continue
  cand="$(all_calls_file "$f" \
          | grep -E "(^|[^a-zA-Z0-9_])(${probe_alt})([^a-zA-Z0-9_]|$)" || true)"
  for probe in $probes; do
    printf '%s\n' "$numbered" | grep -qE "(^|[^a-zA-Z0-9_])${probe}([^a-zA-Z0-9_]|$)" || continue
    # All mentions, so each violation is annotated at ITS OWN line rather than at the probe's
    # canonical add_library. Under $GITHUB_ACTIONS the single-line form put a ::error marker on
    # correct code; the audio guard already attributes per hit.
    mentions="$(printf '%s\n' "$numbered" | grep -E "(^|[^a-zA-Z0-9_])${probe}([^a-zA-Z0-9_]|$)" || true)"
    line="$(printf '%s\n' "$mentions" | head -1 | cut -d: -f1 || true)"
    named=0
    _seen=0
    if [ -n "$cand" ]; then
      while IFS= read -r call; do
        [ -z "$call" ] && continue
        printf '%s\n' "$call" | grep -qE "(^|[^a-zA-Z0-9_])${probe}([^a-zA-Z0-9_]|$)" || continue
        named=1
        _seen=$((_seen + 1))
        cmd="$(printf '%s\n' "$call" | call_name)"
        first="$(printf '%s\n' "$call" | tll_target)"
        # Legal iff ALL THREE hold: the call is in the REGISTRY, it is one of the two allowed
        # commands, and the probe is its TARGET (first argument). The file condition is not
        # decoration -- a cross-directory target_link_libraries naming the probe is CMake's
        # accumulating append, so it is a second link line however canonical it looks in isolation;
        # and being mentioned as an argument to somebody else's call is not a declaration either.
        if [ "$f" = "$PROBE_REGISTRY" ] && [ "$first" = "$probe" ]; then
          case "$cmd" in
            add_library|target_link_libraries) continue ;;
          esac
        fi
        # _seen advances for EVERY call naming this probe, legal ones included, because `mentions`
        # lists every mention in line order -- the legal add_library and target_link_libraries among
        # them. Advancing only on violations indexed the Nth illegal call at the Nth MENTION, so a
        # bad target_compile_options at line 655 was annotated at 653, the legal add_library: exactly
        # the defect this attribution was added to fix. (A single call naming the probe twice would
        # still drift by one; no such call exists or would be legal.)
        _ln="$(printf '%s\n' "$mentions" | sed -n "${_seen}p" | cut -d: -f1 || true)"
        violations="${violations}${f}:${_ln:-${line:-1}}: ${probe} is named by ${cmd}(...) -- only its own add_library and its single target_link_libraries may name a boundary probe. If a probe legitimately needs another command, that is a deliberate widening of this guard: add the command to the allowlist in $0 with a comment saying why, exactly as widening ALLOWED_FILE is treated elsewhere
"
      done <<< "$cand"
    fi
    if [ "$named" -eq 0 ]; then
      # The name is in the file but in no call at all: a bare variable assignment, an if(), a string.
      violations="${violations}${f}:${line:-1}: ${probe} is named outside any command -- only its own add_library and its single target_link_libraries may name a boundary probe
"
    fi
  done
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

echo "boundary-probe guard: OK -- ${count} probe targets verified in ${PROBE_REGISTRY}, each linking exactly one aero:: library PRIVATE and named by nothing but its own add_library and that one target_link_libraries; ${swept} other CMake files swept"

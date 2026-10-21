#!/usr/bin/env bash
# Aero Engine -- the script quickjs-boundary architecture guard (task 4.1.1; docs/04's guard table, the
# ninth of the named architecture guards; risk R12's named next case).
#
# THE INVARIANT (project rule #3): quickjs-ng is reachable from exactly one side of the tree --
# engine/script's PRIVATE side, i.e. every path under engine/script/ that is NOT under
# engine/script/include/. No quickjs include or identifier may appear in any public engine header (the
# layer's own public headers included), nor anywhere else under engine/ or runtime/.
#
# TWO ARMS, over the tracked C-family files under engine/ and runtime/ minus the private side:
#   1. an #include of a quickjs header (quickjs.h, quickjs-libc.h), on the raw text;
#   2. a quickjs identifier, applied AFTER stripping `//` comments.
#
# WHY A SCRIPT, NOT A ONE-LINE `git grep` (the check-platform-boundary.sh precedent). quickjs has no
# namespace: its functions and macros are JS_-prefixed and its types JS<Upper>... -- indistinguishable, AS
# TEXT, from the same identifier cited in a documentation comment, and the script layer cites backend
# functions in `//` prose for traceability. So `//` comments are stripped before the identifier check.
#
# WHY THE IDENTIFIER ARM IS AN ALLOWLIST, NOT A LIST OF SPELLINGS (boundary-guards.md, lesson 0). The
# backend's public names are the JS_ functions and macros and the JS<Upper>... types, and the
# JS<Upper><Upper> ones are real too (JSCFunction..., JSCClosure..., JSGCObjectHeader, JSSABTab -- every
# JS-prefixed name in quickjs.h that does not match JS[A-Z][a-z]). Enumerating them would be a denylist the
# next backend version outgrows. The inverted rule: EVERY token matching JS[A-Z_][A-Za-z0-9_]*, bounded on
# the left, is a violation UNLESS it starts with JSON. The exception set is measured, not guessed:
#     git grep -hoE '(^|[^a-zA-Z0-9_])JS[A-Za-z0-9_]*' -- engine runtime
# returns only JSON and a bare JS in a comment, and the rule needs a third character.
#
# THE COMPILE-TIME HALF is tests/script_boundary_probe.cpp, which links aero::script alone. It cannot see a
# forward declaration (`struct JSContext;` compiles anywhere with no include at all) and this script can;
# this script cannot see anything the compiler resolves through a macro, and the probe's include closure
# covers the umbrella header. Neither is interchangeable with the other (risk R12, docs/08-risks.md:
# vcpkg's shared per-triplet include/ root resolves <quickjs.h> for any target linking any vcpkg package).
#
# Run it locally before pushing:
#     bash .github/scripts/check-script-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.
# Exit codes: 0 clean, 1 violation, 2 the guard cannot verify itself.

set -euo pipefail

# The two trees project rule #1 and the boundary rule govern. editor/, tools/, samples/ and tests/ are
# deliberately out of scope: tests/ links the backend-free allocator seam and reads script sources as text.
readonly SCAN_ROOTS=('engine' 'runtime')

# The private side: engine/script/ minus its public include/ tree. Everything else is scanned, which is why
# engine/script's own public headers are held to the same rule as every other public header.
readonly PRIVATE_SIDE_PREFIX='engine/script/'
readonly PUBLIC_SUBTREE_PREFIX='engine/script/include/'

# A real #include of a quickjs header (quickjs.h, quickjs-libc.h). Anchored: a real directive is the first
# token on its line, and no first-party comment writes an actual `#include <quickjs.h>` line.
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]quickjs'

# Every tracked C-family source under SCAN_ROOTS. .c is IN: this is a dependency guard, not a lint
# (check-golden-rule.sh's reasoning), so excluding a file type would be a permanent blind spot.
c_family() {
  case "$1" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) return 0 ;;
    *) return 1 ;;
  esac
}

# Is this file on engine/script's private side?
is_private_side() {
  case "$1" in
    "${PUBLIC_SUBTREE_PREFIX}"*) return 1 ;;
    "${PRIVATE_SIDE_PREFIX}"*) return 0 ;;
    *) return 1 ;;
  esac
}

# stdin: "<n>:<comment-stripped text>" lines. stdout: "<n>:<token>" for every quickjs-shaped token.
# The text is prefixed with one space so the left boundary is always a real character: no `^` inside an
# awk alternation, whose support differs between the one-true-awk on macOS and gawk/mawk on Linux. match()
# and substr() are POSIX and BSD-safe -- no \b, no -P.
scan_tokens() {
  awk '{
    n = $0; sub(/:.*/, "", n); rest = " " substr($0, length(n) + 2)
    while (match(rest, /[^A-Za-z0-9_]JS[A-Z_][A-Za-z0-9_]*/)) {
      tok = substr(rest, RSTART + 1, RLENGTH - 1)
      if (tok !~ /^JSON/) print n ":" tok
      rest = substr(rest, RSTART + RLENGTH)
    }
  }'
}

cd "$(git rev-parse --show-toplevel)"

# --- Pass 1: the scan set, derived from the tree. -----------------------------------------------------
scanned=0
public_scanned=0
private_files=0
canary_files=0
scanned_subsystems=""
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  if is_private_side "$file"; then
    private_files=$((private_files + 1))
    if grep -qE "$INCLUDE_RE" "$file"; then
      canary_files=$((canary_files + 1))
    fi
    continue
  fi
  scanned=$((scanned + 1))
  case "$file" in
    engine/*/include/*) public_scanned=$((public_scanned + 1)) ;;
  esac
  case "$file" in
    engine/*/*) scanned_subsystems="${scanned_subsystems}$(printf '%s' "$file" | sed -n 's|^engine/\([^/]*\)/.*|\1|p')
" ;;
  esac
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")

# --- Self-test 1: the scan set must not be empty. -------------------------------------------------------
# Without this, a typo in SCAN_ROOTS or c_family turns the guard into a permanent green light.
if [ "$scanned" -eq 0 ]; then
  echo "::error::script-boundary guard scanned 0 files -- it is vacuous. Fix SCAN_ROOTS/c_family in $0." >&2
  exit 2
fi

# --- Self-test 1b: the scan set must COVER the tree, not merely be non-empty. ---------------------------
# "scanned > 0" is still satisfied after SCAN_ROOTS is narrowed to one subsystem, so a leak in every OTHER
# subsystem becomes invisible while the guard prints OK -- the 2.1.2 review's false-green. Both sides are
# derived from the tree: the left holds every engine/<subsystem> with any tracked C-family file, the right
# the subsystems the scan actually visited. `script` is on both only because its public headers are
# scanned, so widening the private side to all of engine/script/ empties it from the right and exits 2.
expected_subsystems=""
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  expected_subsystems="${expected_subsystems}$(printf '%s' "$file" | sed -n 's|^engine/\([^/]*\)/.*|\1|p')
"
done < <(git ls-files -z -- 'engine')
expected_sorted="$(printf '%s' "$expected_subsystems" | sort -u)"
scanned_sorted="$(printf '%s' "$scanned_subsystems" | sort -u)"
missing="$(comm -23 <(printf '%s\n' "$expected_sorted") <(printf '%s\n' "$scanned_sorted"))"
if [ -n "$missing" ]; then
  echo "::error::script-boundary guard's scan set does not cover every engine subsystem that has C-family" >&2
  echo "::error::files -- these are a silent blind spot: $(printf '%s ' $missing)" >&2
  echo "::error::SCAN_ROOTS or the private-side predicate in $0 has been narrowed or widened. Restore it." >&2
  exit 2
fi

# --- Self-test 2: the canary, derived. ----------------------------------------------------------------
# At least one private-side file MUST still include a quickjs header (today engine/script/src/vm_impl.hpp).
# If the layer's directory is renamed, or the regex rots, the guard refuses to pass rather than guarding
# nothing. No file name is hard-coded.
if [ "$canary_files" -eq 0 ]; then
  echo "::error::script-boundary guard found no file under ${PRIVATE_SIDE_PREFIX} (outside include/) that" >&2
  echo "::error::includes a quickjs header -- was the layer renamed, or has INCLUDE_RE rotted? The guard" >&2
  echo "::error::cannot self-verify, so it is refusing to pass. Fix the prefixes or INCLUDE_RE in $0." >&2
  exit 2
fi

# --- Self-test 3: the machinery, on synthetic input. --------------------------------------------------
for line in 'JSValue v;' 'struct JSContext;' 'JS_FreeValue(ctx, v);' 'JSCFunction* f;' 'JS_EVAL_TYPE_GLOBAL'; do
  if [ -z "$(printf '1:%s\n' "$line" | scan_tokens)" ]; then
    echo "::error::scan_tokens in $0 no longer reports a quickjs token in '${line}' -- it is vacuous." >&2
    exit 2
  fi
done
for line in 'const char* k = "JSON";' 'JsonValue v;' 'auto j = parseJSON(t);'; do
  if [ -n "$(printf '1:%s\n' "$line" | scan_tokens)" ]; then
    echo "::error::scan_tokens in $0 reports a token in '${line}' -- it over-matches; fix the regex." >&2
    exit 2
  fi
done
if [ -n "$(printf '1:%s\n' '// wraps JS_NewRuntime2' | sed -E 's|//.*||' | scan_tokens)" ]; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi
for line in '#include <quickjs.h>' '#  include "quickjs-libc.h"'; do
  if ! printf '%s\n' "$line" | grep -qE "$INCLUDE_RE"; then
    echo "::error::INCLUDE_RE in $0 no longer matches '${line}' -- it is vacuous." >&2
    exit 2
  fi
done
if printf '%s\n' '// see quickjs.h' | grep -qE "$INCLUDE_RE"; then
  echo "::error::INCLUDE_RE in $0 matches a comment -- fix the regex." >&2
  exit 2
fi

# --- The guard. -------------------------------------------------------------------------------------------
violations=""
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  is_private_side "$file" && continue

  case "$file" in
    engine/*/include/*) where="in a public engine header" ;;
    *) where="outside engine/script's private side" ;;
  esac

  # Line-numbered, comment-stripped view for the identifier arm: a documentation citation of a real backend
  # name never trips the guard, while an actual declaration does. `nl -ba -w1 -s:` matches the sibling scripts
  # (BSD-safe).
  hits="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||' | scan_tokens)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      tok="${hit#*:}"
      violations="${violations}${file}:${n}: quickjs identifier '${tok}' ${where}
"
    done <<< "$hits"
  fi

  # The #include arm runs on the raw file: a real directive is never inside a comment in this codebase's style.
  inc_hits="$(grep -nE "$INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: quickjs header included ${where}
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")

if [ -n "$violations" ]; then
  echo "quickjs leaked outside engine/script's private side -- task 4.1.1 / docs/04 boundary rule:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "quickjs-ng is touched only by engine/script/src/ (and any engine/script/ path outside include/)." >&2
  echo "Expose an engine type through aero/script/*.hpp instead." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::quickjs leaked outside engine/script's private side (task 4.1.1; docs/04 boundary rule). Expose an engine type through aero/script/*.hpp instead."
    done <<< "$violations"
  fi
  exit 1
fi

echo "script-boundary guard: OK -- ${scanned} tracked engine/runtime C-family files scanned (${public_scanned} public headers among them); quickjs confined to engine/script's private side (${private_files} files)"

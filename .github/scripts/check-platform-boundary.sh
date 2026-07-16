#!/usr/bin/env bash
# Aero Engine — the platform SDL-boundary architecture guard (task 0.3.1; docs/04's guard table).
#
# Rule #3 (boundary rule): no SDL type may cross a public engine header. engine/platform links
# SDL3 PRIVATE, confined to engine/platform/src/platform.cpp; tests/platform_boundary_probe.cpp is
# the compile-time half of the guard. This script is the textual half, reaching where the probe
# cannot (vcpkg's shared per-triplet include/ root still resolves <SDL3/...> for any target linking
# any vcpkg package -- risk R12, docs/08-risks.md).
#
# WHY A SCRIPT, NOT A ONE-LINE `git grep` (deviation from the plan's §3.17 draft, logged in the
# 0.3.1 implementation report): SDL has no namespace, so a real SDL identifier (SDL_Init, SDL_GPU,
# SDL_Window, SDL_HINT_VIDEO_DRIVER, ...) is indistinguishable, AS TEXT, from that same identifier
# cited in a documentation comment -- and this codebase already cites exact SDL identifiers in
# prose for traceability (engine/core/include/aero/core/math.hpp mentions SDL_GPU; time.hpp mentions
# SDL_GetTicks; this task's own context.hpp/window.hpp cite SDL_Init/SDL_Window/SDL_HINT_* to justify
# their design). A plain `(^|[^a-zA-Z0-9_])SDL_` grep (the spdlog/enkiTS precedent) fires on ALL of
# those -- verified: it already matches on `main`, before this task added a single line, entirely
# from pre-existing math/time comments. spdlog/enkiTS do not have this problem: `spdlog::`/`enki::`
# and `TaskScheduler.h` essentially never appear in first-party prose. SDL_-prefixed identifiers do,
# constantly, because that IS how you write a comment that documents SDL behaviour. This script
# strips `//` line comments before applying the identifier check, exactly the disambiguation the
# spec's own grep comment already claimed ("leaving the bare word SDL in prose/comments alone") but
# a bare grep cannot actually deliver.
#
# Run it locally before pushing:
#     bash .github/scripts/check-platform-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# A real #include of an SDL3 header. Anchored (a real directive is the first token on its line,
# same reasoning as check-math-boundary.sh's INCLUDE_RE) -- no first-party comment writes an actual
# `#include <SDL3/...>` line, so this alone would already be safe unanchored, but anchoring costs
# nothing and matches the sibling script's style.
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]SDL3/'

# A real SDL_-prefixed identifier used as CODE: the bracket form (not \b -- BSD grep on macOS
# degrades \b to a literal 'b', [[posix-ere-word-boundary-trap]]) applied AFTER stripping any `//`
# line comment, so a documentation citation like "-- verified SDL_init.h:81" never matches, while an
# actual `SDL_Window* leak;` declaration does.
readonly IDENTIFIER_RE='(^|[^a-zA-Z0-9_])SDL_'

# Every public engine header -- the only surface the boundary rule governs (docs/04: "public headers
# expose only engine types"). Deliberately narrower than the clang-format file set: this guard has no
# opinion on engine/platform/src/platform.cpp, which is SDL's one legitimate home.
readonly HEADER_GLOB='engine/*/include/*'

cd "$(git rev-parse --show-toplevel)"

# --- Self-test 1: the scan set must not be empty. -------------------------------------------
scanned=$(git ls-files -- "$HEADER_GLOB" | wc -l | tr -d '[:space:]')
if [ "$scanned" -eq 0 ]; then
  echo "::error::platform-boundary guard scanned 0 files -- it is vacuous. Fix HEADER_GLOB in $0." >&2
  exit 2
fi

# --- Self-test 2: the machinery still fires (no allowlisted file exists to use as a canary, unlike
# check-math-boundary.sh -- every public header must be SDL-free, so this proves the regexes against
# synthetic input instead). ------------------------------------------------------------------------
if ! printf '#include <SDL3/SDL.h>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::INCLUDE_RE in $0 no longer matches a real SDL3 include -- it is vacuous." >&2
  exit 2
fi
if ! printf 'SDL_Window* leak;\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 no longer matches a real SDL identifier -- it is vacuous." >&2
  exit 2
fi
if printf '// verified SDL_init.h:81\n' | sed 's|//.*||' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi

# --- The guard. ----------------------------------------------------------------------------
violations=""
while IFS= read -r -d '' file; do
  # Line-numbered, comment-stripped view of this file: strip `// ...` before applying
  # IDENTIFIER_RE, so a documentation citation of a real SDL name never trips the guard, while an
  # actual SDL_-prefixed declaration (never legitimately preceded by `//` on the same line) does.
  hits="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||' | grep -E "$IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: SDL identifier leaked into a public engine header
"
    done <<< "$hits"
  fi
  # The #include check runs on the raw file (a real directive is never inside a comment in this
  # codebase's style, and anchoring already protects against indented/prefixed false matches).
  inc_hits="$(grep -nE "$INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: SDL3 header included from a public engine header
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "$HEADER_GLOB")

if [ -n "$violations" ]; then
  echo "SDL leaked into a public engine header -- task 0.3.1 / docs/04 boundary rule:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "Keep SDL inside engine/platform/src/platform.cpp." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::SDL leaked into a public engine header (task 0.3.1; docs/04 boundary rule). Keep SDL inside engine/platform/src/platform.cpp."
    done <<< "$violations"
  fi
  exit 1
fi

echo "platform-boundary guard: OK -- ${scanned} tracked public headers scanned; SDL confined to engine/platform/src/platform.cpp"

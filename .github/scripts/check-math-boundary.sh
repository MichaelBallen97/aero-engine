#!/usr/bin/env bash
# Aero Engine — the math-boundary architecture guard (task 0.2.3; docs/04's guard table).
#
# ADR-005: the engine owns its math types so the backend stays swappable (GLM -> RTM is deferred
# to Phase 8, docs/08). That promise is only real if GLM is reachable from exactly ONE file.
#
# CMake links glm::glm PRIVATE, which already makes `#include <glm/...>` a hard compile error for
# engine-layer targets that link no vcpkg package directly (tests/math_boundary_probe.cpp is the
# permanent proof of that). But vcpkg installs every port into ONE flat per-triplet include/ that
# lands on the compile line of ANY target linking ANY vcpkg package -- tests/ inherits it via
# doctest::doctest and CAN resolve <glm/...> (risk R12, docs/08-risks.md). This script is the
# enforcement that reaches everywhere the compile-time boundary cannot.
#
# Run it locally before pushing:
#     bash .github/scripts/check-math-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# THE one file allowed to include GLM. ADR-005's exit door is exactly one file wide: the RTM swap
# replaces this file and nothing else. If you are here to add a second entry, read the ADR-005
# implementation note in docs/02-adrs.md first -- widening this is a design change, not a chore.
readonly ALLOWED_FILE='engine/core/src/math/glm_backend.cpp'

# A GLM include, and nothing else.
#   ^[[:space:]]*  -- anchored: a real directive is the first token on its line, while every prose
#                     mention of this rule (math.hpp:9, math_test.cpp:12, tests/CMakeLists.txt:24)
#                     sits behind a `//` or `#`. Anchoring is what keeps those comments legal.
#   #[[:space:]]*include -- `#  include` is legal C++.
#   [<"]           -- `#include "glm/x.hpp"` compiles exactly like the angle form.
# Deliberately NOT matching `glm::`: no TU can name glm:: without a first-party ancestor including
# a GLM header, and that ancestor is caught here -- while 7 legitimate comments do say `glm::`.
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]glm/'

# Every tracked C-family source. A deliberate superset of the clang-format step's list: .mm/.m
# arrive with the iOS/macOS entry points (0.3.1, 5.x) and must not be a blind spot on arrival.
# git ls-files never descends into the vcpkg submodule, so GLM's own headers are never scanned,
# and it lists tracked files only, so build/ and vcpkg_installed/ are structurally excluded.
EXTS=('*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.hxx' '*.inl' '*.mm' '*.m')

cd "$(git rev-parse --show-toplevel)"

# --- Self-test 1: the scan set must not be empty. -------------------------------------------
# Without this, a typo in EXTS or the pathspec turns the guard into a permanent green light.
scanned=$(git ls-files -- "${EXTS[@]}" ":!:$ALLOWED_FILE" | wc -l | tr -d '[:space:]')
if [ "$scanned" -eq 0 ]; then
  echo "::error::math-boundary guard scanned 0 files -- it is vacuous. Fix EXTS/pathspec in $0." >&2
  exit 2
fi

# --- Self-test 2: the canary. --------------------------------------------------------------
# The one allowlisted file MUST still match the regex. If GLM's include style changes, or the
# backend is renamed, or the regex rots, this trips -- instead of the guard passing forever.
if [ ! -f "$ALLOWED_FILE" ]; then
  echo "::error::Allowlisted backend '$ALLOWED_FILE' is missing. Was it renamed? The guard cannot" >&2
  echo "         self-verify, so it is refusing to pass. Update ALLOWED_FILE in $0." >&2
  exit 2
fi
if ! grep -qE "$INCLUDE_RE" "$ALLOWED_FILE"; then
  echo "::error::The guard's regex matches no GLM include in '$ALLOWED_FILE' -- it is vacuous and" >&2
  echo "         would pass regardless of leaks. Fix INCLUDE_RE in $0." >&2
  exit 2
fi

# --- The guard. ----------------------------------------------------------------------------
# `|| true` is safe here precisely BECAUSE the two self-tests above already proved the machinery
# fires: grep exits 1 (and xargs 123) on the no-match path, which is the good path.
violations="$(git ls-files -z -- "${EXTS[@]}" ":!:$ALLOWED_FILE" \
              | xargs -0 -r grep -HnE "$INCLUDE_RE" || true)"

if [ -n "$violations" ]; then
  echo "GLM leaked outside the backend -- ADR-005 / docs/04 math-boundary guard (task 0.2.3):" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "Only $ALLOWED_FILE may include GLM." >&2
  echo "Fix: add the operation to that backend and expose it as an engine:: type through" >&2
  echo "     <aero/core/math.hpp>. Never widen the boundary to make a call site convenient." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      f="${v%%:*}"; r="${v#*:}"; n="${r%%:*}"
      echo "::error file=${f},line=${n}::GLM include outside ${ALLOWED_FILE} (ADR-005). Add the op to the backend and expose an engine:: type via <aero/core/math.hpp>."
    done <<< "$violations"
  fi
  exit 1
fi

echo "math-boundary guard: OK -- ${scanned} tracked files scanned; GLM confined to ${ALLOWED_FILE}"

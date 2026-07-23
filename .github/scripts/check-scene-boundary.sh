#!/usr/bin/env bash
# Aero Engine — the scene EnTT-boundary architecture guard (task 1.3.1; docs/04's guard table).
#
# THE INVARIANT: no entt/EnTT/ENTT token appears in any public engine header. This is the task's
# deliverable verbatim, and it is the boundary rule's own wording ("no third-party type crosses the
# engine's public API").
#
# WHY A SCRIPT, NOT AN INLINE grep (the spdlog/enkiTS precedent): the scene headers document
# entt::basic_registry in first-party prose for traceability (world.hpp's own file header, and this
# script's own comments below), so a bare `entt::` grep would fire on documentation. The script strips
# `//` line comments before the identifier check, exactly as check-platform-boundary.sh and
# check-rhi-boundary.sh do.
#
# WHY PUBLIC HEADERS ONLY, NOT SOURCES (unlike check-rhi-boundary.sh): docs/03 designates
# engine/reflect the future "entt::meta runtime", so a source-level single-layer confinement would
# pre-emptively outlaw a documented future layer. Widening the scope is a separate, deliberate
# decision — reopen it when engine/reflect actually grows its entt dependency (spec §6).
#
# engine/scene/internal/** and engine/scene/src/** are STRUCTURALLY out of scope (neither matches
# engine/*/include/*), which is why no allowlist is needed.
#
# Run it locally before pushing:
#     bash .github/scripts/check-scene-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

readonly HEADER_GLOB='engine/*/include/*'

# The seam header — the guard's CANARY. It is the one file in the tree guaranteed to contain a real
# entt include, so if it is renamed or the regex rots, the guard refuses to pass instead of passing
# vacuously. It is NOT under HEADER_GLOB, so it is never itself scanned.
readonly CANARY_FILE='engine/scene/internal/aero/scene/internal/world_access.hpp'

# A real #include of an entt header (they all live under entt/).
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]entt/'

# A real entt identifier used as CODE, applied AFTER stripping `//` comments. The bracket boundary
# (^|[^a-zA-Z0-9_]) -- NOT \b, which BSD grep on macOS degrades to a literal 'b'
# ([[posix-ere-word-boundary-trap]]). Two arms: the namespace (entt::) and the macro/config prefix
# (ENTT_). Does NOT match the word "entt" in prose (no :: and no _), nor "EnTT::EnTT" in a CMake file
# (not scanned).
readonly IDENTIFIER_RE='(^|[^a-zA-Z0-9_])(entt::|ENTT_)'

cd "$(git rev-parse --show-toplevel)"

# --- Self-test 1: the scan set must not be empty. -------------------------------------------
scanned=$(git ls-files -- "$HEADER_GLOB" | wc -l | tr -d '[:space:]')
if [ "$scanned" -eq 0 ]; then
  echo "::error::scene-boundary guard scanned 0 files -- it is vacuous. Fix HEADER_GLOB in $0." >&2
  exit 2
fi

# --- Self-test 2: the canary. ----------------------------------------------------------------
if [ ! -f "$CANARY_FILE" ]; then
  echo "::error::Canary '$CANARY_FILE' is missing. Was the seam renamed? The guard cannot self-verify," >&2
  echo "         so it is refusing to pass. Update CANARY_FILE in $0." >&2
  exit 2
fi
if ! grep -qE "$INCLUDE_RE" "$CANARY_FILE"; then
  echo "::error::The guard's regex matches no entt include in '$CANARY_FILE' -- it is vacuous and" >&2
  echo "         would pass regardless of leaks. Fix INCLUDE_RE in $0." >&2
  exit 2
fi

# --- Self-test 3: the machinery fires, and does NOT over-match documentation prose. -----------
if ! printf 'entt::basic_registry<X> r;\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 no longer matches a real entt identifier -- it is vacuous." >&2
  exit 2
fi
if ! printf '#include <entt/entt.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::INCLUDE_RE in $0 no longer matches a real entt include -- it is vacuous." >&2
  exit 2
fi
if printf '// the entt::basic_registry lives in Impl\n' | sed 's|//.*||' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi
if printf 'the entity storage is different\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 over-matches the bare word 'entity' -- fix the regex." >&2
  exit 2
fi

# --- The guard. ----------------------------------------------------------------------------
violations=""
while IFS= read -r -d '' file; do
  # Line-numbered, comment-stripped view for the identifier check: strip `// ...` before applying
  # IDENTIFIER_RE, so a documentation citation of a real entt name never trips the guard while an
  # actual entt::basic_registry declaration does. nl -ba -w1 -s: matches check-platform-boundary.sh
  # and check-rhi-boundary.sh (BSD-safe).
  stripped="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: entt identifier leaked into a public engine header
"
    done <<< "$hits"
  fi

  # The #include check runs on the raw file (a real directive is never inside a comment in this
  # codebase's style, and anchoring protects against indented/prefixed false matches).
  inc_hits="$(grep -nE "$INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: <entt/...> included in a public engine header
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "$HEADER_GLOB")

if [ -n "$violations" ]; then
  echo "entt leaked into a public engine header -- task 1.3.1 / docs/04 boundary rule:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "Fix: route it through the engine::World API. If you are AUTHORING or REGISTERING a component" >&2
  echo "     type, include <aero/scene/internal/world_access.hpp> from a NON-PUBLIC TU (link" >&2
  echo "     aero::scene_internal)." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::entt leaked into a public engine header (task 1.3.1; docs/04 boundary rule). Route it through engine::World, or through <aero/scene/internal/world_access.hpp> from a non-public TU (aero::scene_internal)."
    done <<< "$violations"
  fi
  exit 1
fi

echo "scene-boundary guard: OK -- ${scanned} tracked public engine headers scanned; entt confined to engine/scene/src/world.cpp + ${CANARY_FILE}"

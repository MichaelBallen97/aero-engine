#!/usr/bin/env bash
# Aero Engine — the RHI SDL_GPU-boundary architecture guard (task 0.4.5; docs/04's guard table, the
# fifth of the five named architecture guards). ADR-002 "the sacred wrapper".
#
# THE INVARIANT: SDL_GPU is reachable from exactly ONE translation unit in engine/ + runtime/ --
# engine/rhi/src/sdl_gpu_backend.cpp. That one file is the escape hatch the v3-v4 ray-tracing path
# will replace; nothing else in engine/ or runtime/ may name an SDL_GPU type, call an SDL_GPU
# function, or include <SDL3/SDL_gpu.h> (project rule #3 / ADR-002).
#
# WHY THIS IS NOT check-platform-boundary.sh's job. That guard scans public HEADERS
# (engine/*/include/*) and already rejects every SDL_ identifier there -- SDL_GPU* included, since
# SDL_GPU* is a subset of SDL_. What it does NOT cover is engine/runtime SOURCE files: a render or
# runtime .cpp could #include <SDL3/SDL_gpu.h> and call SDL_CreateGPUBuffer directly, bypassing the
# RHI, and the platform header-scan would never see it. THIS guard is the sacred-wrapper enforcement
# that reaches those .cpp files -- the SDL_GPU analogue of check-math-boundary.sh's repo-wide GLM
# scan (allowlist one file), NOT of check-platform-boundary.sh's header-only scan.
#
# WHY A SCRIPT WITH COMMENT-STRIPPING, NOT A ONE-LINE grep (the platform-guard precedent). SDL has no
# namespace, and this codebase cites SDL_GPU identifiers in first-party PROSE for traceability
# (math.hpp/transform.hpp's NDC notes; device.hpp "the ONLY place SDL_GPU exists";
# format.hpp/types.hpp/rhi.hpp; native_window.hpp/platform.cpp's SDL_ClaimWindowForGPUDevice). A bare
# SDL_GPU grep fires on all of those -- it already matches on `main`, entirely from those comments.
# So the guard strips `//` line comments before the identifier check, letting an actual
# `SDL_GPUDevice* d;` fail while a doc citation does not.
#
# WHY SDL_GPU-SPECIFIC TOKENS, NOT bare SDL_. engine/platform/src/platform.cpp legitimately uses
# SDL_Init / SDL_Window / SDL_CreateWindow -- non-GPU SDL is the platform layer's job (guarded by
# check-platform-boundary.sh's header scan, not here). This guard matches ONLY the SDL_GPU surface:
# the SDL_gpu.h include, and identifiers of the form SDL_<...>GPU (covers types SDL_GPUDevice, enums
# SDL_GPU_*, AND functions SDL_CreateGPUBuffer / SDL_AcquireGPUCommandBuffer /
# SDL_ClaimWindowForGPUDevice, where GPU follows the verb).
#
# Run it locally before pushing:
#     bash .github/scripts/check-rhi-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# THE one file allowed to touch SDL_GPU. ADR-002's exit door is exactly one TU wide: the ray-tracing
# backend swap (v3-v4) replaces this file and nothing else. NOTE this is an exact-file allowlist, not
# a directory: engine/rhi/src/format.cpp -- a sibling in the same src/ -- is NOT allowlisted and is
# scanned. Widening this is a design change (read ADR-002 in docs/02-adrs.md first), not a chore.
readonly ALLOWED_FILE='engine/rhi/src/sdl_gpu_backend.cpp'

# The two trees the sacred-wrapper invariant governs (docs/03 layers). editor/, tools/, samples/ and
# tests/ are deliberately OUT of scope: editor/tools live on the far side of the boundary rule, and
# tests are white-box (aero_tests links SDL3 directly and names "SDL_GPU caps" in a doctest case).
# Public headers are a SUBSET of engine/, so this scope subsumes the header-only reading; the
# compile-time probe (tests/rhi_boundary_probe.cpp) covers the public headers at compile time too.
readonly SCAN_ROOTS=('engine' 'runtime')

# A real #include of the SDL_GPU header (flat under SDL3/). Anchored: a real directive is the first
# token on its line, and no first-party comment writes an actual `#include <SDL3/SDL_gpu.h>` line.
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]SDL3/SDL_gpu\.h'

# A real SDL_GPU identifier used as CODE, applied AFTER stripping `//` comments. The bracket boundary
# (^|[^a-zA-Z0-9_]) -- NOT \b, which BSD grep on macOS degrades to a literal 'b'
# ([[posix-ere-word-boundary-trap]]) -- then SDL_, then any run of identifier chars, then GPU. Matches
# SDL_GPUDevice (0 chars before GPU), SDL_GPU_TEXTUREFORMAT_* (0 chars, the enum prefix), and
# SDL_CreateGPUBuffer / SDL_AcquireGPUCommandBuffer / SDL_ClaimWindowForGPUDevice (verb before GPU).
# Does NOT match SDL_Init / SDL_Window (no GPU) or a `// SDL GPU` prose comment (space, and stripped).
readonly IDENTIFIER_RE='(^|[^a-zA-Z0-9_])SDL_[A-Za-z0-9]*GPU'

# Every tracked C-family source under SCAN_ROOTS. Same extension set as check-math-boundary.sh: .mm/.m
# are in for the iOS/macOS runtime entry points (Phase 5) that must not be a blind spot on arrival.
# .c is out -- the only .c in the tree is vendored miniaudio_impl.c (SDL_GPU-free), auto-excluded like
# the linters do. git ls-files never descends into the vcpkg submodule and lists tracked files only,
# so build/ and vcpkg_installed/ are structurally excluded.
c_family() {
  case "$1" in
    *.cpp|*.hpp|*.h|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) return 0 ;;
    *) return 1 ;;
  esac
}

cd "$(git rev-parse --show-toplevel)"

# --- Self-test 1: the scan set must not be empty. -------------------------------------------
# Without this, a typo in SCAN_ROOTS or c_family turns the guard into a permanent green light.
scanned=0
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  [ "$file" = "$ALLOWED_FILE" ] && continue
  scanned=$((scanned + 1))
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")
if [ "$scanned" -eq 0 ]; then
  echo "::error::rhi-boundary guard scanned 0 files -- it is vacuous. Fix SCAN_ROOTS/c_family in $0." >&2
  exit 2
fi

# --- Self-test 2: the canary. --------------------------------------------------------------
# The one allowlisted backend MUST still include SDL_GPU. If it is renamed, or the regex rots, this
# trips -- instead of the guard passing forever.
if [ ! -f "$ALLOWED_FILE" ]; then
  echo "::error::Allowlisted backend '$ALLOWED_FILE' is missing. Was it renamed? The guard cannot" >&2
  echo "         self-verify, so it is refusing to pass. Update ALLOWED_FILE in $0." >&2
  exit 2
fi
if ! grep -qE "$INCLUDE_RE" "$ALLOWED_FILE"; then
  echo "::error::The guard's regex matches no SDL_gpu.h include in '$ALLOWED_FILE' -- it is vacuous" >&2
  echo "         and would pass regardless of leaks. Fix INCLUDE_RE in $0." >&2
  exit 2
fi

# --- Self-test 3: the machinery fires, and does NOT over-match non-GPU SDL. ------------------
if ! printf 'SDL_GPUDevice* leak;\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 no longer matches a real SDL_GPU type -- it is vacuous." >&2
  exit 2
fi
if ! printf 'auto* b = SDL_CreateGPUBuffer(d);\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 no longer matches an SDL_GPU function -- it is vacuous." >&2
  exit 2
fi
if printf '// the ONLY place SDL_GPU exists\n' | sed 's|//.*||' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi
if printf 'SDL_Init(SDL_INIT_VIDEO);\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::IDENTIFIER_RE in $0 over-matches non-GPU SDL (SDL_Init) -- fix the regex." >&2
  exit 2
fi

# --- The guard. ----------------------------------------------------------------------------
violations=""
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  [ "$file" = "$ALLOWED_FILE" ] && continue

  # Line-numbered, comment-stripped view for the identifier check: strip `// ...` before applying
  # IDENTIFIER_RE, so a documentation citation of a real SDL_GPU name never trips the guard while an
  # actual SDL_GPU declaration does. `nl -ba -w1 -s:` matches check-platform-boundary.sh (BSD-safe).
  stripped="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: SDL_GPU identifier used outside the backend
"
    done <<< "$hits"
  fi

  # The #include check runs on the raw file (a real directive is never inside a comment in this
  # codebase's style, and anchoring protects against indented/prefixed false matches).
  inc_hits="$(grep -nE "$INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: <SDL3/SDL_gpu.h> included outside the backend
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")

if [ -n "$violations" ]; then
  echo "SDL_GPU leaked outside the backend -- ADR-002 / docs/04 rhi-boundary guard (task 0.4.5):" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "Only $ALLOWED_FILE may touch SDL_GPU." >&2
  echo "Fix: add the capability to the RHI Device API (device.hpp + sdl_gpu_backend.cpp) and call it" >&2
  echo "     through engine::rhi types. Never reach past the wrapper (ADR-002, the escape hatch)." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::SDL_GPU used outside ${ALLOWED_FILE} (ADR-002, the sacred wrapper). Route it through the engine::rhi::Device API instead."
    done <<< "$violations"
  fi
  exit 1
fi

echo "rhi-boundary guard: OK -- ${scanned} tracked engine/runtime sources scanned; SDL_GPU confined to ${ALLOWED_FILE}"

#!/usr/bin/env bash
# Aero Engine — the golden-rule architecture guard (task 2.1.2; docs/00 / docs/03 "The golden rule";
# docs/04's guard table, third of the five named architecture guards). Project rule #1:
#
#     The editor depends on the engine. The engine NEVER depends on the editor.
#
# `/editor` became a populated, ImGui-linking tree exactly one commit ago (task 2.1.1). Before that
# commit the rule was unbreakable by construction -- there was nothing to depend on. It is now
# breakable in ways an #include scan alone cannot see (see below), which is why this guard is only
# the TEXTUAL half. The LINK-graph and INCLUDE-DIRECTORY half is cmake/golden_rule.cmake's
# aero_assert_golden_rule(), which runs inside every configure on every lane. Neither half subsumes
# the other.
#
# WHY A SCRIPT, NOT AN INLINE `git grep` (D2 / F2). engine/ + runtime/ already carry 22 (as of
# 2026-07-25 -- do not treat that count as pinned) first-party PROSE citations of the word "editor",
# every one of them a `//` or `#` comment (e.g. engine/CMakeLists.txt's own "GOLDEN RULE: nothing in
# this tree may ever depend on editor/."). A bare `editor` token grep is therefore permanently red.
# Comment-stripping is mandatory for the namespace arm below, and comment-stripping requires a
# script -- the identical trap that forced 0.3.1's SDL guard (check-platform-boundary.sh) to be a
# script rather than a one-liner.
#
# WHY THE INCLUDE ARM MATCHES A PATH SEGMENT, NOT JUST AN `aero/editor/` PREFIX (F5). editor/'s public
# headers are PUBLIC on aero_editor_core, and no engine or runtime target consumes that target -- so
# an ABSOLUTE `#include <aero/editor/x.hpp>` already fails to compile everywhere. What DOES compile
# today is the RELATIVE ESCAPE, `#include "../../../editor/include/aero/editor/x.hpp"`, resolved
# relative to the including file. That is the hole this arm closes, which is why it must match
# `editor` as a whole path segment anywhere in the path (never just a prefix) -- and why
# `text_editor.hpp` / `editor_support/` must stay legal.
#
# WHY THE NAMESPACE ARM EXISTS (D6 / F16). The editor's namespace is `engine::editor`, and this
# codebase already uses cross-layer FORWARD DECLARATION as a technique in live code
# (imgui_layer.hpp / editor_ui.hpp forward-declare `namespace engine::rhi` / `engine::platform`).
# The mirror-image move -- an engine file forward-declaring an editor type with NO #include at all --
# is therefore a live hole, not a theoretical one, and no include scan can ever see it.
#
# THE INVERTED CANARY (D8). Every other boundary guard in this tree anchors its self-test on an
# ALLOWLISTED file that legitimately uses the thing being banned. There is no such file here --
# nothing under engine/ or runtime/ may EVER reference the editor -- so the canary is inverted:
# editor/include/aero/editor/ (the tree this guard protects AGAINST) must still hold at least one
# tracked file, and a synthetic include of that file's basename must still match INCLUDE_RE. Renaming
# or relocating the editor's public headers therefore trips exit 2 instead of silently guarding
# nothing.
#
# Local invocation:
#     bash .github/scripts/check-golden-rule.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# The two trees project rule #1 governs. editor/, tools/, samples/ and tests/ are deliberately OUT of
# scope: tests/ links aero::editor_core on purpose (aero_editor_imgui_test, task 2.1.1), and tools/
# lives on the far side of the dependency-placement invariant. Combined for anti-vacuity below (D3).
readonly SCAN_ROOTS=('engine' 'runtime')

# D8's inverted canary directory -- see the header comment above.
readonly CANARY_DIR='editor/include/aero/editor'

# Arm 1 -- an #include whose path contains `editor` as a whole PATH SEGMENT, applied to the RAW file
# (D5): a real directive is never inside a comment in this codebase's style, and the
# ^[[:space:]]*# anchor independently defeats a `//`-prefixed citation (F4 probe 8). The optional
# group `([^">]*/)?` MUST end in '/' -- that terminal slash is what keeps text_editor.hpp and
# editor_support/ legal.
readonly INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^">]*/)?editor/'

# Arm 2 -- an editor namespace named as CODE, applied AFTER stripping `//` comments (D6): this arm
# has NO anchor, so stripping is mandatory, not defence-in-depth -- a comment citing
# `engine::editor::ImGuiLayer` would otherwise trip it (2 of the 22 F2 citations are near-misses of
# exactly this shape). Bracket boundaries (^|[^a-zA-Z0-9_]) ... ([^a-zA-Z0-9_]|$), NEVER \b -- BSD/
# macOS ERE degrades \b to a literal 'b', silently voiding the arm (repo memory
# posix-ere-word-boundary-trap; this exact bug shipped once at 0.2.4 and was found at 0.2.5). Two
# spellings: `engine::editor` (the real one, F16) and `namespace<ws>editor` (the nested-block form
# `namespace engine { namespace editor {`). Deeper spellings are an accepted documented residual.
readonly IDENTIFIER_RE='(^|[^a-zA-Z0-9_])(engine::editor|namespace[[:space:]]+editor)([^a-zA-Z0-9_]|$)'

# Every tracked C-family source under SCAN_ROOTS. UNLIKE check-rhi-boundary.sh, .c is IN: this is a
# DEPENDENCY guard, not a lint, so excluding a file type would create a permanent blind spot for a
# future first-party .c/.m entry point (Phase 5 ships iOS/Android runtime entry points into this very
# tree). Scanning the vendored engine/platform/src/miniaudio_impl.c costs nothing and can never match.
c_family() {
  case "$1" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) return 0 ;;
    *) return 1 ;;
  esac
}

cd "$(git rev-parse --show-toplevel)"

# --- Self-test group 1: scan set non-empty (combined, never per-root). ----------------------------
# runtime/ legitimately has ZERO sources today (F7) -- a per-root check would fail on a correct tree.
# This is spec §5's "single most likely implementation bug in the task".
scanned=0
while IFS= read -r -d '' file; do
  c_family "$file" || continue
  scanned=$((scanned + 1))
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")
if [ "$scanned" -eq 0 ]; then
  echo "::error::golden-rule guard scanned 0 files under engine/+runtime -- cannot self-verify. Fix SCAN_ROOTS/c_family in $0." >&2
  exit 2
fi

# --- Self-test group 2: the inverted canary is present. --------------------------------------------
canary_files="$(git ls-files -- "$CANARY_DIR")"
if [ -z "$canary_files" ]; then
  echo "::error::golden-rule guard: no tracked files under '$CANARY_DIR' -- was the editor tree renamed? cannot self-verify." >&2
  exit 2
fi
canary_first="$(printf '%s\n' "$canary_files" | head -n1)"
canary_base="$(basename "$canary_first")"

# --- Self-test group 3: the canary still matches INCLUDE_RE. ---------------------------------------
# Ties INCLUDE_RE to the real tree layout so the regex cannot rot into a no-op while the tree still
# looks right.
if ! printf '#include <aero/editor/%s>\n' "$canary_base" | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE no longer matches a synthetic include of the canary '$canary_base' -- cannot self-verify. Fix INCLUDE_RE in $0." >&2
  exit 2
fi

# --- Self-test group 4: positive probes (all 7 must MATCH). ----------------------------------------
# Include arm.
if ! printf '#include <aero/editor/imgui_layer.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE no longer matches the canonical absolute form -- cannot self-verify." >&2
  exit 2
fi
if ! printf '#include "../../editor/include/aero/editor/editor_ui.hpp"\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE no longer matches the relative-escape form (the only form that compiles today) -- cannot self-verify." >&2
  exit 2
fi
if ! printf '#include <editor/foo.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE no longer matches 'editor' as the first path segment -- cannot self-verify." >&2
  exit 2
fi
if ! printf '  #  include   "aero/editor/x.hpp"\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE no longer tolerates odd spacing -- cannot self-verify." >&2
  exit 2
fi
# Namespace arm.
if ! printf 'namespace engine::editor { class ImGuiLayer; }\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE no longer matches the forward-declaration hole -- cannot self-verify." >&2
  exit 2
fi
if ! printf 'engine::editor::ImGuiLayer* layer;\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE no longer matches an engine::editor use -- cannot self-verify." >&2
  exit 2
fi
if ! printf 'namespace editor {\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE no longer matches the nested-block spelling -- cannot self-verify." >&2
  exit 2
fi

# --- Self-test group 5: negative probes (all 6 must NOT match) + the stripping proof. --------------
# Include arm.
if printf '#include <aero/core/text_editor.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE over-matches a substring ('text_editor.hpp' is not a path segment) -- fix the regex." >&2
  exit 2
fi
if printf '#include <aero/editor_support/x.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE over-matches a segment PREFIX ('editor_support/') -- fix the regex." >&2
  exit 2
fi
if printf '#include <aero/editor.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE over-matches a FILE named editor.hpp (not a directory) -- fix the regex." >&2
  exit 2
fi
if printf '// engine code must never #include <aero/editor/x.hpp>\n' | grep -qE "$INCLUDE_RE"; then
  echo "::error::golden-rule guard: INCLUDE_RE matches a //-prefixed citation -- the ^[[:space:]]*# anchor is broken." >&2
  exit 2
fi
# Namespace arm.
if printf 'engine::editorial x;\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE over-matches 'editorial' (right boundary broken) -- fix the regex." >&2
  exit 2
fi
if printf 'my_engine::editor\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE over-matches 'my_engine::editor' (left boundary broken) -- fix the regex." >&2
  exit 2
fi
if printf 'the editor converts at the UI boundary\n' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: IDENTIFIER_RE over-matches bare prose -- fix the regex." >&2
  exit 2
fi
# The stripping proof (the direct analogue of check-rhi-boundary.sh's comment-stripping self-test).
if printf '// engine::editor::ImGuiLayer\n' | sed -E 's|//.*||' | grep -qE "$IDENTIFIER_RE"; then
  echo "::error::golden-rule guard: comment-stripping is broken -- a pure comment line still matches IDENTIFIER_RE." >&2
  exit 2
fi

# --- The scan. ---------------------------------------------------------------------------------
violations=""
while IFS= read -r -d '' file; do
  c_family "$file" || continue

  # Include arm runs on the RAW file (D5).
  inc_hits="$(grep -nE "$INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: #include reaches into /editor
"
    done <<< "$inc_hits"
  fi

  # Namespace arm runs on the comment-stripped, line-numbered view (D6).
  stripped="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: editor namespace named in engine code
"
    done <<< "$hits"
  fi
  # `|| true` is safe here because self-test groups 4-5 already proved the machinery fires --
  # the check-math-boundary.sh precedent for the identical pattern.
done < <(git ls-files -z -- "${SCAN_ROOTS[@]}")

if [ -n "$violations" ]; then
  echo "The engine must never depend on the editor -- project rule #1 / docs/04 golden-rule guard (task 2.1.2):" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "The dependency must point the other way: move the type into engine/, or invert the call with a" >&2
  echo "callback/observer seam -- see aero::platform_internal's RawEventAccessor" >&2
  echo "(engine/platform/internal/aero/platform/internal/native_event.hpp) for the sanctioned shape." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::The engine must never depend on the editor (project rule #1). Move the type into engine/, or invert the call with a callback/observer seam."
    done <<< "$violations"
  fi
  exit 1
fi

echo "golden-rule guard: OK -- ${scanned} tracked engine/runtime sources scanned; no /editor dependency"

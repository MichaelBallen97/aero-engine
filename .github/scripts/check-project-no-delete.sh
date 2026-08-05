#!/usr/bin/env bash
# Aero Engine — the project-no-delete architecture guard (task 2.6.1, code-review round; D7/INV-P4).
#
# INV-P4 (.claude/rules/editor.md's "Projects" section): a createProject FAILURE never deletes
# anything it already created, on ANY failure path, EVER -- the target directory, assets/, scenes/,
# whatever exists at the moment of failure, all stay exactly as they are. The reviewer accepted
# sabotage seed S11 (the WriteFailed branch's rollback is genuinely unreachable by any test in this
# tree -- the directoryIsEmpty gate at the adoption step always fires first) as DOCUMENTED DEBT, on
# ONE condition: the only artefact that proved a future `remove_all` would be caught was §V7's grep in
# the task's own plan file -- and `docs/plans/` is GITIGNORED, so that grep ceases to exist the moment
# this branch merges. This script is what replaces "the safety-critical branch is unproven" with "the
# offending call cannot be written at all", which is strictly stronger and, unlike the plan's grep,
# survives the merge.
#
# THE INVARIANT: none of `remove_all`, `std::filesystem::remove`, `std::filesystem::rename`, or a
# bare `::copy` (covers `std::filesystem::copy` and an `fs::copy` alias alike) may appear as CODE in
# editor/src/project.cpp, editor/src/project_file.cpp, editor/src/project_ui.cpp,
# editor/src/asset_meta.cpp, editor/src/asset_database.cpp or editor/src/asset_cache.cpp -- the six
# files that own every filesystem-writing line in the project flow (D7), the asset flow (task 3.1.1's
# D7 "an invalid .meta is never overwritten" / D8 "an orphan is never deleted"), and the import cache
# (task 3.1.2's D18). Widened from three files to five by task 3.1.1, now to SIX by task 3.1.2: the
# same unreachable-by-test problem sabotage seed S11 documented for createProject's rollback branch
# applies identically to these flows, so they get the same enforcement rather than a second, parallel
# guard. Task 3.1.2's nuance, stated because it reads like a contradiction with the asset cache's own
# D15 (the cache's DATA is disposable): nothing in 3.1.2 deletes a FILE to dispose of stale cache data
# -- a discard means "do not carry the entries forward", a rebuild means an atomic overwrite via the
# same `writeTextFileAtomic` primitive project_file.cpp and asset_meta.cpp already use. Listing
# `asset_cache.cpp` costs nothing today and stops a future "clean the Library folder" `remove_all`
# from being written without a review; if such an action is ever wanted, it must be a deliberate,
# reviewed relaxation that deletes ONLY inside `Library/` and never inside the assets tree. This is
# deliberately an ALLOWLIST OF NAMED FILES, not a glob over editor/src/ -- the invariant is specific to
# these flows, and a glob would also flag legitimate deletion elsewhere in the editor (e.g. a future
# asset-deletion feature) that neither D7 says anything about. Note that the SCRIPT's NAME
# ("project-no-delete") is narrower than its SCOPE for historical reasons -- a rename was considered
# and rejected (D15/A12 of task 3.1.1's plan, reaffirmed by task 3.1.2) because it would touch the
# workflow YAML step name, the hermetic ctest case name, CLAUDE.md and .claude/rules/editor.md, each a
# place a rename can go half-done; widening the allowlist costs none of that. `editor/src/text_file.cpp`
# is deliberately NOT in this list -- it is the shared atomic-write primitive that every flow calls
# `writeTextFileAtomic` through, and its internal `rename` is the mechanism that makes an atomic write
# atomic. The invariant governs what the flow files may call directly, not the primitive they call.
#
# WHY COMMENT-STRIPPED (the check-scene-boundary.sh / check-rhi-boundary.sh precedent): a future
# maintainer explaining WHY a call is forbidden, in a comment, must not itself trip the guard it is
# describing.
#
# Run it locally before pushing:
#     bash .github/scripts/check-project-no-delete.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# The six files D7/INV-P4 (project flow), task 3.1.1's D7/D8 (asset flow) and task 3.1.2's D18
# (import cache) govern. Order matches the "Files touched" table (project.hpp, asset_meta.hpp/
# asset_database.hpp/asset_cache.hpp are PURE and <filesystem>-free by construction, so they are not
# in this list at all; nothing to scan there).
readonly FORBIDDEN_FILES=(
  "editor/src/project.cpp"
  "editor/src/project_file.cpp"
  "editor/src/project_ui.cpp"
  "editor/src/asset_meta.cpp"       # task 3.1.1 (D7/D8)
  "editor/src/asset_database.cpp"   # task 3.1.1 (D7/D8)
  "editor/src/asset_cache.cpp"      # task 3.1.2 (D18)
)

# remove_all | std::filesystem::remove | std::filesystem::rename | ::copy -- exactly the four tokens
# the code-review finding names. `std::filesystem::remove` also matches the PREFIX of
# `std::filesystem::remove_all`, so the two alternatives overlap deliberately rather than needing a
# fifth, narrower pattern.
readonly FORBIDDEN_RE='(remove_all|std::filesystem::remove|std::filesystem::rename|::copy)'

cd "$(git rev-parse --show-toplevel)"

# --- Self-test 1: every named file must exist and be TRACKED. -------------------------------------
# Without this, a rename of any of the six (project_file.cpp -> project_fs.cpp, say) would silently
# shrink the scan to fewer files instead of refusing to pass.
for f in "${FORBIDDEN_FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "::error::project-no-delete guard: '$f' is missing. Was it renamed? The guard cannot self-verify," >&2
    echo "         so it is refusing to pass. Update FORBIDDEN_FILES in $0." >&2
    exit 2
  fi
  if [ -z "$(git ls-files -- "$f")" ]; then
    echo "::error::project-no-delete guard: '$f' exists but is not tracked by git -- the guard scans" >&2
    echo "         tracked files only ('git add' it), so it is refusing to pass rather than silently" >&2
    echo "         scanning nothing for this file." >&2
    exit 2
  fi
done

# --- Self-test 2: the regex actually fires on each of the four forbidden forms. --------------------
for probe in \
  'std::filesystem::remove_all(target, ec);' \
  'std::filesystem::remove(target, ec);' \
  'std::filesystem::rename(target, dest, ec);' \
  'std::filesystem::copy(target, dest, ec);'; do
  if ! printf '%s\n' "$probe" | grep -qE "$FORBIDDEN_RE"; then
    echo "::error::project-no-delete guard: FORBIDDEN_RE in $0 no longer matches '$probe' -- it is" >&2
    echo "         vacuous for that form. Fix FORBIDDEN_RE." >&2
    exit 2
  fi
done

# --- Self-test 3: comment-stripping is load-bearing and does not over-match. -----------------------
if printf '// never call std::filesystem::remove_all here (D7/INV-P4)\n' | sed 's|//.*||' | grep -qE "$FORBIDDEN_RE"; then
  echo "::error::project-no-delete guard: comment-stripping in $0 is broken -- a pure comment line" >&2
  echo "         still matches." >&2
  exit 2
fi
if printf 'the target directory may need to be removed by the USER, by hand\n' | grep -qE "$FORBIDDEN_RE"; then
  echo "::error::project-no-delete guard: FORBIDDEN_RE in $0 over-matches ordinary prose containing" >&2
  echo "         the bare word 'removed' -- fix the regex." >&2
  exit 2
fi

# --- The guard. --------------------------------------------------------------------------------
violations=""
for f in "${FORBIDDEN_FILES[@]}"; do
  # Line-numbered, comment-stripped view, exactly the check-scene-boundary.sh / check-rhi-boundary.sh
  # shape (BSD-safe: nl -ba -w1 -s, then strip `//...`).
  stripped="$(nl -ba -w1 -s: "$f" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$FORBIDDEN_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${f}:${n}: a filesystem delete/rename/copy call in the project/asset/cache flow (D7/INV-P4; task 3.1.1 D7/D8; task 3.1.2 D18)
"
    done <<< "$hits"
  fi
done

if [ -n "$violations" ]; then
  echo "A delete/rename/copy call leaked into the project, asset or cache flow -- task 2.6.1 D7/INV-P4, task 3.1.1 D7/D8 and task 3.1.2 D18:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "Fix: a createProject failure, an invalid .meta, an orphaned .meta, or a stale cache entry must" >&2
  echo "     NEVER be removed, renamed or copied on ANY path. Leave it inert and report the" >&2
  echo "     failure/finding instead (see .claude/rules/editor.md's \"Projects (task 2.6.1)\" and" >&2
  echo "     \"Assets (task 3.1.1)\" sections)." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::a filesystem delete/rename/copy call leaked into the project, asset or cache flow (task 2.6.1 D7/INV-P4; task 3.1.1 D7/D8; task 3.1.2 D18). Never remove/rename/copy anything these flows must leave alone."
    done <<< "$violations"
  fi
  exit 1
fi

# --- Check B (task 3.1.3, D13): the POSITIVE half. -------------------------------------------------
# Check A above is a DENYLIST of six named files, which means a delete written into a SEVENTH file
# passes it silently. This check closes that hole: across EVERY tracked editor/src/*.cpp, a
# filesystem delete or rename may appear only in the two files that are allowed to have one.
#
# `::copy` is deliberately NOT in this pattern, and the reason is not the one Check A has. `::copy`
# matches `std::copy` -- the ordinary <algorithm> call. Check A's six files are hand-picked and none
# of them calls it (verified: `git grep -nE '::copy' -- 'editor/src/*.cpp'` is empty today), but a
# GLOB over every editor/src TU would false-positive the first time anyone writes one, and a guard
# that cries wolf is a guard that gets relaxed.
readonly DELETE_RE='(remove_all|std::filesystem::remove|std::filesystem::rename)'
readonly PERMITTED_DELETERS=(
  "editor/src/text_file.cpp"      # the atomic-write primitive: its rename IS what makes a write atomic
  "editor/src/asset_actions.cpp"  # task 3.1.3 (D12): the ONE user-initiated orphan-sidecar deletion
)

# --- B-self-test 1: the regex fires on each of the three forbidden forms, and NOT on std::copy. ----
for probe in \
  'std::filesystem::remove_all(target, ec);' \
  'std::filesystem::remove(target, ec);' \
  'std::filesystem::rename(target, dest, ec);'; do
  if ! printf '%s\n' "$probe" | grep -qE "$DELETE_RE"; then
    echo "::error::project-no-delete guard (Check B): DELETE_RE in $0 no longer matches '$probe' -- it is" >&2
    echo "         vacuous for that form. Fix DELETE_RE." >&2
    exit 2
  fi
done
if printf '%s\n' 'std::copy(a, b, out);' | grep -qE "$DELETE_RE"; then
  echo "::error::project-no-delete guard (Check B): DELETE_RE in $0 over-matches std::copy -- fix the regex." >&2
  exit 2
fi

# --- B-self-test 2: comment-stripping is load-bearing and does not over-match. ---------------------
if printf '// never call std::filesystem::remove here\n' | sed 's|//.*||' | grep -qE "$DELETE_RE"; then
  echo "::error::project-no-delete guard (Check B): comment-stripping in $0 is broken -- a pure comment" >&2
  echo "         line still matches." >&2
  exit 2
fi

# --- B-self-test 3 (the important one): every PERMITTED_DELETERS entry must exist and be tracked. --
# Renaming asset_actions.cpp must fail the guard LOUDLY (exit 2), not silently widen the check to
# "nobody is permitted, and nothing matched, so we pass" (seed S24).
for f in "${PERMITTED_DELETERS[@]}"; do
  if [ ! -f "$f" ]; then
    echo "::error::project-no-delete guard (Check B): '$f' is missing. Was it renamed? The guard cannot self-verify," >&2
    echo "         so it is refusing to pass. Update PERMITTED_DELETERS in $0." >&2
    exit 2
  fi
  if [ -z "$(git ls-files -- "$f")" ]; then
    echo "::error::project-no-delete guard (Check B): '$f' exists but is not tracked by git -- refusing" >&2
    echo "         to pass rather than silently scanning nothing for this file." >&2
    exit 2
  fi
done

is_permitted() {
  local candidate="$1"
  for permitted in "${PERMITTED_DELETERS[@]}"; do
    if [ "$candidate" = "$permitted" ]; then
      return 0
    fi
  done
  return 1
}

checkBViolations=""
checkBScanned=0
while IFS= read -r f; do
  [ -z "$f" ] && continue
  checkBScanned=$((checkBScanned + 1))
  if is_permitted "$f"; then
    continue
  fi
  stripped="$(nl -ba -w1 -s: "$f" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$DELETE_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      checkBViolations="${checkBViolations}${f}:${n}: a filesystem delete/rename call outside the two permitted files (task 3.1.3 D13)
"
    done <<< "$hits"
  fi
done < <(git ls-files -- 'editor/src/*.cpp')

# code-review finding 7 (boundary-guards.md's "every guard needs an anti-vacuity canary"): Check B's
# own scan count was printed but never ASSERTED non-zero, and the two allowlisted files are SKIPPED by
# the scan loop above -- so nothing proved the `git ls-files -- 'editor/src/*.cpp'` glob returned
# anything at all. A future move of editor/src (or a typo in the glob) would leave this printing
# "0 ... scanned" and exiting 0, silently disabling Check B entirely.
if [ "$checkBScanned" -eq 0 ]; then
  echo "::error::project-no-delete guard (Check B): 0 files matched 'editor/src/*.cpp' -- refusing to" >&2
  echo "         pass on an empty scan (anti-vacuity canary; boundary-guards.md)." >&2
  exit 2
fi

if [ -n "$checkBViolations" ]; then
  echo "A delete/rename call was found in a file NOT in Check B's PERMITTED_DELETERS allowlist (task 3.1.3 D13):" >&2
  echo "$checkBViolations" >&2
  echo "" >&2
  echo "Fix: only editor/src/text_file.cpp (the atomic-write rename) and editor/src/asset_actions.cpp" >&2
  echo "     (the one sanctioned orphan-sidecar delete) may call remove_all/std::filesystem::remove/" >&2
  echo "     std::filesystem::rename. A new destructive path is a deliberate, reviewed change." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::a filesystem delete/rename call outside the two permitted files (task 3.1.3 D13)."
    done <<< "$checkBViolations"
  fi
  exit 1
fi

echo "project-no-delete guard: OK -- Check A: ${#FORBIDDEN_FILES[@]} files scanned, no delete/rename/copy in the project, asset or cache flow; Check B: ${checkBScanned} editor/src/*.cpp scanned, delete/rename confined to ${#PERMITTED_DELETERS[@]} permitted files"

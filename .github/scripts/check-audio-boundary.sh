#!/usr/bin/env bash
# Aero Engine — the audio-boundary architecture guard (task 3.7.3; docs/04's guard table; epic 3.7's
# Definition of Done: "no miniaudio type is public (guard-enforced)").
#
# THE INVARIANT, IN TWO HALVES:
#
#   1. THE NO-VCPKG PROPERTY. aero_assets, aero_audio and aero_scene_audio link NO vcpkg package at
#      all, which is what makes their PRIVATE links a REAL compile-time boundary rather than
#      convention-plus-grep (R12, docs/08-risks.md: vcpkg installs every port into ONE shared
#      per-triplet include/ root that lands on the compile line of any target linking any vcpkg
#      package). Each CMakeLists says "NO find_package. NOT ONE, EVER." in its own header — and
#      adding one anyway voids the property SILENTLY WHILE CI STAYS GREEN. This guard is what makes
#      that sentence enforceable: the link line makes the boundary green TODAY by construction; a
#      guard is what proves it STAYS green, which is a different job.
#
#   2. THE MINIAUDIO TOKEN BAN. No miniaudio token — <miniaudio.h> or a ma_-prefixed identifier used
#      as code — anywhere under engine/audio/ or engine/scene_audio/, sources INCLUDED. miniaudio's
#      only legal engine home is engine/platform/src/{audio_device.cpp, miniaudio_impl.c} (guarded
#      there by check-platform-boundary.sh + tests/platform_boundary_probe.cpp since 0.3.3); its only
#      other home is editor/src/audio_decode.cpp, on the far side of the boundary rule.
#
# WHY engine/assets IS IN AN "AUDIO-BOUNDARY" SCRIPT — the question the next reader will have. The
# no-vcpkg property is ONE invariant across THREE files, not an audio one: all three carry the same
# "NO find_package. NOT ONE, EVER." prohibition header, the same R12 rationale and the same voiding
# vector, and CLAUDE.md's standing ritual read all three as a single grep. engine/assets additionally
# holds half the audio pipeline (cooked_audio.cpp, audio_cook.cpp). The guard lives with the audio
# epic because 3.7.3 is the task the invariant's own comments hand it to by name; guarding two of the
# three files would have left a known manual ritual half-automated forever.
#
# WHY THE TEXTUAL HALF IS NOT REDUNDANT WITH THE COMPILE ERROR. The hard-compile-error property holds
# only where the compile line is vcpkg-free — and in the *-release presets it is NOT: the three
# targets link aero::profiling PRIVATE, an INTERFACE library that carries Tracy::TracyClient (and
# with it the whole shared vcpkg include root) when AERO_ENABLE_PROFILING=ON. A stray
# `#include <miniaudio.h>` in engine/audio therefore compiles CLEAN in Release and fails only in
# Debug. This guard reddens it on every push, in every configuration, before any compiler runs.
#
# WHY A SCRIPT WITH COMMENT-STRIPPING, NOT A ONE-LINE grep (the platform/rhi/scene precedent): the
# audio headers cite ma_device_uninit / ma_spatializer in first-party `//` prose (audio.hpp,
# system.hpp, spatial.hpp — the lifetime rule and the recorded ma_spatializer deviation), and the
# three CMakeLists each carry the words "find_package" and "#include <miniaudio.h>" inside their own
# prohibition comments. A bare grep is permanently red on a clean tree; CLAUDE.md carried that as a
# "grep that must be READ rather than counted" ritual, and this script is that reading, mechanized:
# `#`/`//` comments are stripped before every identifier/command check.
#
# GENERATOR EXPRESSIONS ARE AN EXIT 1, NOT AN EVASION, AND THAT IS DELIBERATE. 2.1.2 found a genex
# hole in the golden-rule link walk ($<$<CONFIG:Debug>:aero::editor_core> evaded a naive tokeniser),
# so it is worth saying where this guard stands: LINK_TOKEN_RE is an ALLOWLIST anchored at both ends,
# so a genex-shaped token on one of these three link lines matches nothing and is reported as "not an
# aero:: engine target". There is no genex arm because none is needed — the fail-loud direction is
# already the default here. Wanting a genex on one of these lines is a conscious guard edit.
#
#
# BRACKET COMMENTS ARE NOT STRIPPED, and that is a deliberate fail-loud choice both guards make
# identically. `strip_cmake` removes `#` line comments only, so the continuation lines of a
# `#[[ ... ]]` block comment are read as code and a guarded name or banned command inside one is
# reported. Neither style is used anywhere in this tree. The alternative -- parsing bracket comments
# -- would mean a real violation could be hidden inside one, which is the wrong direction for a
# guard; a false positive is a two-line edit, a false negative is another review round.
# Run it locally before pushing:
#     bash .github/scripts/check-audio-boundary.sh
# NOTE: it scans TRACKED files only -- `git add` a new file before expecting it to be seen.
#
# bash 3.2 compatible on purpose (macOS ships 3.2.57): no mapfile, no associative arrays.

set -euo pipefail

# The three vcpkg-free CMakeLists — the targets whose PRIVATE links are a real compile-time boundary.
# assets is here alongside the two audio targets because the property is ONE invariant across all
# three (CLAUDE.md's standing find_package grep covered all three as one ritual); the guard lives
# with the audio epic because 3.7.3 is the task the invariant's own comments hand it to.
# A FOURTH VCPKG-FREE TARGET MUST ADD ITSELF HERE IN THE COMMIT THAT CREATES IT: intent cannot be
# derived from the tree, so an unlisted target is silently unguarded. THIS IS THE ONLY ROSTER --
# VCPKG_FREE_TARGETS and Part 1d's skip test are both derived from it, deliberately, so there is no
# second or third list to forget. Self-test 1a checks the derivation against each file's own
# add_library, so a subsystem that breaks the naming convention cannot slip through it.
readonly VCPKG_FREE_CMAKE=('engine/assets/CMakeLists.txt' 'engine/audio/CMakeLists.txt' 'engine/scene_audio/CMakeLists.txt')

# The two audio subsystems the miniaudio token ban governs, sources included (NOT just public
# headers: check-platform-boundary.sh already rejects ma_ in every engine/*/include/* header; what it
# does not cover is an engine/audio .cpp reaching for a device type — the exact shape
# engine/audio/CMakeLists.txt's own header warns 3.7.2 could void it with).
readonly AUDIO_ROOTS=('engine/audio' 'engine/scene_audio')

# CMake commands that can put a vcpkg (or any external) header/library onto these targets' compile
# or link lines. Matched case-insensitively ON COMMENT-STRIPPED TEXT — CMake command names are
# case-insensitive, and the prohibition comments themselves contain "find_package". include_directories
# / link_directories / link_libraries (the directory-scoped forms) and add_subdirectory are banned
# outright: none has a legitimate use in these three files, and a future legitimate need edits this
# guard consciously (the ALLOWED_FILE precedent: widening is a design change, not a chore).
#
# `include` IS IN THE LIST AND IT IS NOT A ROUNDING ERROR. Parts 1a/1b/1c read exactly the three
# rostered files, and Part 1d looks only for target_link_libraries, so NOTHING follows an
# `include(...)`: a two-line edit -- `include(${CMAKE_CURRENT_SOURCE_DIR}/audio_deps.cmake)` here plus
# a find_package and a foreign include dir in that file -- voided the whole of prong A while this
# guard printed its OK banner (measured, 3.7.3's code-review round). `include(cmake/*.cmake)` is a
# live idiom in this tree (the root CMakeLists.txt uses it five times), so the vector has in-tree
# precedent, which is exactly the bar the other entries here are held to.
#
# THE LEADING (^|[^a-zA-Z0-9_]) IS LOAD-BEARING, NOT DECORATION: target_include_directories( and
# target_link_libraries( CONTAIN the banned substrings include_directories( and link_libraries(, so
# without it this guard is permanently red on a clean tree. `include` needs the SAME protection twice
# over -- from target_include_directories( and from a path ending in /include) -- and gets it from the
# trailing [[:space:]]*\(, since neither is followed by an opening paren. Self-test 3 pins all of it.
#
# set_target_properties / set_property ARE BANNED HERE TOO, and for a reason that generalises past
# any one property: EVERY predicate this guard enforces has a property spelling. LINK_LIBRARIES is
# target_link_libraries' underlying property, INCLUDE_DIRECTORIES is target_include_directories', and
# `set_property(TARGET aero_audio APPEND PROPERTY LINK_LIBRARIES miniaudio)` inside a guarded file put
# a vcpkg package on the link line while every arm below read clean (measured, 3.7.3's second
# code-review round). Rather than chase one property at a time, the commands themselves are refused:
# none of the three files uses either today, and neither has a legitimate need. THIS IS THE GENERAL
# FORM OF THE LESSON THE FIRST ROUND TAUGHT -- match the predicate, not the spelling in front of you.
readonly BANNED_CMAKE_RE='(^|[^a-zA-Z0-9_])(find_package|find_path|find_library|find_file|find_program|pkg_check_modules|pkg_search_module|include_directories|link_directories|link_libraries|add_subdirectory|include|set_target_properties|set_property)[[:space:]]*\('

# The three guarded targets, DERIVED from VCPKG_FREE_CMAKE rather than listed a second time. A
# separate roster is a silent-green drift direction: a fourth vcpkg-free target added to the file
# list but not to the target list would be guarded by Parts 1a/1b/1c and INVISIBLE to Part 1d, with
# no test able to notice. engine/<name>/CMakeLists.txt -> aero_<name> is this tree's convention, and
# self-test 1 asserts each derived name is really declared by an add_library in its own file, so a
# future subsystem that breaks the convention is a loud exit 2 rather than a quiet blind spot.
VCPKG_FREE_TARGETS=''
for _f in "${VCPKG_FREE_CMAKE[@]}"; do
  _d="${_f%/CMakeLists.txt}"
  VCPKG_FREE_TARGETS="${VCPKG_FREE_TARGETS}aero_${_d##*/} "
done
readonly VCPKG_FREE_TARGETS

# A real #include of the miniaudio header (flat: the vcpkg port installs include/miniaudio.h).
readonly MA_INCLUDE_RE='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]miniaudio\.h'
# A real miniaudio identifier used as CODE. Lowercase ma_ is the entire public surface (ma_device,
# ma_decoder, ma_dr_wav, ...); bracket boundary, NOT \b (BSD grep degrades \b to a literal 'b' --
# posix-ere-word-boundary-trap), applied AFTER stripping // comments.
readonly MA_IDENTIFIER_RE='(^|[^a-zA-Z0-9_])ma_'

# Link tokens legal inside the three CMakeLists' target_link_libraries calls: the target itself,
# a visibility keyword, or an aero:: engine target.
#
# ANY *_internal TARGET IS REFUSED FIRST, IN BOTH SPELLINGS, and the "both" is the whole point.
# Those targets carry their backend INTERFACE by design, so linking one restores the very include
# root this file exists to keep off the line. Refusing only the aero::scene_internal ALIAS left the
# RAW names -- aero_scene_internal, aero_platform_internal, aero_rhi_internal, all of which exist in
# engine/{scene,platform,rhi}/CMakeLists.txt -- matching LINK_TOKEN_RE's aero_[a-z_]+ branch and
# passing with the OK banner (measured, 3.7.3's code-review round). The raw name is what anyone
# copying from engine/scene/CMakeLists.txt would write. Matched with the same *_internal glob
# check-boundary-probes.sh uses, so the two guards cannot drift apart.
readonly LINK_TOKEN_RE='^(aero_[a-z_]+|aero::[a-z_]+|PUBLIC|PRIVATE|INTERFACE)$'

# Directory-scoped commands reach a target WITHOUT NAMING IT: CMake's directory properties are
# inherited, so include_directories() / link_libraries() / link_directories() in any ANCESTOR
# CMakeLists apply to every target defined below it. That is the one class the name-based sweep
# cannot see, and it is closed here rather than written down as residual, because no ancestor of the
# three guarded directories uses any of them today.
readonly DIR_SCOPED_RE='(^|[^a-zA-Z0-9_])(include_directories|link_libraries|link_directories)[[:space:]]*\('

# THE ONLY COMMANDS A VCPKG-FREE CMakeLists MAY CONTAIN. This is an ALLOWLIST, and it replaces the
# losing half of a three-round argument: BANNED_CMAKE_RE below enumerates the commands known to be
# dangerous, and three consecutive review rounds each closed the instances it named and left the CLASS
# open -- an alias but not a raw name, an add_library keyword but not a property write, a property
# write but not the plain command. CMake has too many ways to reach a target for a denylist to
# converge. These three commands are what all three files use today and all they have ever needed;
# anything else is a conscious guard edit, exactly like widening ALLOWED_FILE.
readonly GUARDED_FILE_COMMANDS='add_library target_include_directories target_link_libraries'

cd "$(git rev-parse --show-toplevel)"

# Strip CMake `#` line comments. (Bracket comments #[[...]] are not used in this tree; a banned
# command inside one would FALSE-POSITIVE, which is the fail-loud direction for a textual guard.)
strip_cmake() { sed 's|#.*||'; }
flat_file_text() { strip_cmake < "$1" | tr '\n' ' '; }

# Flatten a comment-stripped CMake file to one line and emit every <command>(...) call as one line.
# The three files' calls contain no nested parens and no ')' in a string — self-test 3 pins the
# extractor against exactly the multi-line shape they use today.
extract_calls() {  # $1 = case-insensitive command-name ERE fragment, stdin = file text
  strip_cmake | tr '\n' ' ' | grep -oiE "(^|[^a-zA-Z0-9_])$1[[:space:]]*\([^)]*\)" || true
}

# Is $1 one of the guarded CMakeLists themselves? Derived from the one roster, so Part 1d's skip
# list cannot drift away from the file list the way a hardcoded `case` did.
# THE DIRECTORY-SCOPE SET: every file that can push state into the directory scope a guarded target
# is defined in. CMake's directory properties are inherited, so include_directories() in ANY of these
# reaches the target without ever naming it -- the one direction the naming allowlist structurally
# cannot see.
#
# It is computed, not listed, and it covers two things a hand-written list of `*/CMakeLists.txt`
# ancestors did not (both measured escaping, 3.7.3's fourth review round):
#   * a `.cmake` module `include()`d from an ancestor. The root CMakeLists includes five of them
#     BEFORE add_subdirectory(engine), so `include_directories(...)` in cmake/sanitizers.cmake is
#     inherited by all three guarded targets. That is the `include()` bypass one directory up -- the
#     same vector this script's own header calls the flagship one.
#   * the closure is transitive, so a module that includes another module is covered too.
# The guarded files themselves are NOT in this set: Part 1e already allows them only three commands.
dir_scope_files() {
  _seed=''
  for _g in "${VCPKG_FREE_CMAKE[@]}"; do
    _d="${_g%/CMakeLists.txt}"
    while case "$_d" in */*) true ;; *) false ;; esac; do
      _d="${_d%/*}"
      _seed="${_seed}${_d}/CMakeLists.txt
"
    done
    _seed="${_seed}CMakeLists.txt
"
  done
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

# The regex Part 1d sweeps with: the target name as a whole token. The TRAILING boundary is what
# keeps aero_audio_boundary_probe -- a target this task added inside the swept set -- from reading as
# aero_audio. Hoisted into a function so the self-test below and the sweep cannot use different text.
target_mention_re() { printf '(^|[^a-zA-Z0-9_])%s([^a-zA-Z0-9_]|$)' "$1"; }

# Is $1 one of the guarded CMakeLists themselves? Derived from the one roster, so Part 1d's skip
# test cannot drift away from the file list the way a hardcoded `case` did.
is_guarded_file() {
  for _g in "${VCPKG_FREE_CMAKE[@]}"; do
    [ "$1" = "$_g" ] && return 0
  done
  return 1
}

all_calls() {  # stdin = file text
  strip_cmake | tr '\n' ' ' \
    | grep -oiE '(^|[^a-zA-Z0-9_])[a-zA-Z_][a-zA-Z0-9_]*[[:space:]]*\([^)]*\)' || true
}

# The command name of one extracted call, lower-cased (CMake command names are case-insensitive).
call_name() {  # stdin = one call
  sed -E 's/^[^a-zA-Z_]*//; s/[[:space:]]*\(.*$//' | tr 'A-Z' 'a-z'
}


# --- Self-test 1: the guarded files and roots must all exist and be tracked. -------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  if ! git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
    echo "::error::audio-boundary guard: '$f' is not a tracked file. Was the subsystem moved or" >&2
    echo "         renamed? The guard cannot self-verify, so it is refusing to pass. Update" >&2
    echo "         VCPKG_FREE_CMAKE in $0." >&2
    exit 2
  fi
done
# Self-test 1a: every DERIVED target name is really the target its own CMakeLists declares. This is
# what makes deriving VCPKG_FREE_TARGETS safe instead of merely tidy: a fourth vcpkg-free subsystem
# whose target does not follow engine/<name>/ -> aero_<name> is a loud exit 2 here, rather than a
# target that Parts 1a/1b/1c guard while Part 1d silently cannot see it.
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  d="${f%/CMakeLists.txt}"
  t="aero_${d##*/}"
  if ! grep -qE "(^|[^a-zA-Z0-9_])add_library[[:space:]]*\([[:space:]]*${t}([^a-zA-Z0-9_]|$)" "$f"; then
    echo "::error::audio-boundary guard: '$f' does not declare a target named '${t}', so the" >&2
    echo "         derived VCPKG_FREE_TARGETS entry is wrong and Part 1d would not see it. Either" >&2
    echo "         follow the engine/<name>/ -> aero_<name> convention or teach $0 the mapping." >&2
    exit 2
  fi
done
for root in "${AUDIO_ROOTS[@]}"; do
  n=0
  while IFS= read -r -d '' file; do
    case "$file" in
      *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) n=$((n + 1)) ;;
    esac
  done < <(git ls-files -z -- "$root")
  if [ "$n" -eq 0 ]; then
    echo "::error::audio-boundary guard: root '$root' contains no tracked C-family sources -- the" >&2
    echo "         token scan is vacuous there. Was the subsystem moved or renamed? Update" >&2
    echo "         AUDIO_ROOTS in $0." >&2
    exit 2
  fi
done

# --- Self-test 2: the canaries -- the prohibition prose and the real ma_ citations. ------------
# Each guarded CMakeLists carries its own "NO find_package" prohibition comment, so the RAW text
# must contain the word while the STRIPPED text contains no banned call: the comment is the in-tree
# proof that comment-stripping is doing real work, exactly the reading CLAUDE.md's manual ritual
# prescribed. If a prohibition comment is ever deleted, the guard refuses to pass rather than
# passing with its canary gone.
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  if ! grep -qiE 'find_package' "$f"; then
    echo "::error::audio-boundary guard: '$f' no longer contains its find_package prohibition" >&2
    echo "         comment -- the guard's comment-strip canary is gone. Restore the prohibition" >&2
    echo "         header (see engine/audio/CMakeLists.txt) or update this self-test in $0." >&2
    exit 2
  fi
done
# The audio headers cite ma_ identifiers in `//` prose (audio.hpp / system.hpp / spatial.hpp -- the
# lifetime rule and the ma_spatializer deviation note). At least one RAW citation must exist across
# the scan set, or the identifier regex has rotted / the scan no longer walks the real files.
raw_ma=0
while IFS= read -r -d '' file; do
  case "$file" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) ;;
    *) continue ;;
  esac
  # Tracked-but-absent again: this loop reads every tracked source by name, and grep would print
  # "No such file or directory" to stderr while the guard went on to exit 0 -- noise on a routine
  # rename that the e2e's own S11 stage asserts is absent.
  [ -f "$file" ] || continue
  if grep -qE "$MA_IDENTIFIER_RE" "$file"; then raw_ma=$((raw_ma + 1)); fi
done < <(git ls-files -z -- "${AUDIO_ROOTS[@]}")
if [ "$raw_ma" -eq 0 ]; then
  echo "::error::audio-boundary guard: no raw ma_ citation found anywhere under the audio roots --" >&2
  echo "         the identifier regex or the scan set has rotted (audio.hpp/system.hpp/spatial.hpp" >&2
  echo "         each cite ma_ names in // prose). Fix MA_IDENTIFIER_RE or AUDIO_ROOTS in $0." >&2
  exit 2
fi

# --- Self-test 3: the machinery fires, and does NOT over-match prose. --------------------------
if ! printf 'find_package(miniaudio CONFIG REQUIRED)\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 no longer matches a real find_package call -- vacuous." >&2
  exit 2
fi
if ! printf 'FIND_PACKAGE(Tracy CONFIG REQUIRED)\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 misses an upper-case CMake command -- vacuous (CMake command" >&2
  echo "         names are case-insensitive)." >&2
  exit 2
fi
if printf '# NO find_package. NOT ONE, EVER.\n' | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure comment line still matches." >&2
  exit 2
fi
# The two arms below pin the LEADING bracket boundary of BANNED_CMAKE_RE. target_include_directories(
# and target_link_libraries( CONTAIN the banned substrings include_directories( and link_libraries(,
# and every one of the three guarded files uses both commands: drop the boundary and this guard is
# permanently red on a clean tree. Pinned here rather than left to be rediscovered.
if printf 'target_include_directories(aero_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 over-matches target_include_directories -- the leading" >&2
  echo "         (^|[^a-zA-Z0-9_]) boundary has been lost. It is load-bearing, not decoration." >&2
  exit 2
fi
if printf 'target_link_libraries(aero_audio PUBLIC aero::core)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 over-matches target_link_libraries -- same lost boundary." >&2
  exit 2
fi
# `include` is the flagship bypass and needs pinning in THREE directions: it must fire on a real
# include(), and it must NOT fire on target_include_directories( (the leading bracket) or on a path
# ending in /include) (the trailing paren) -- both of which appear in every one of the three guarded
# files, so either over-match would red the tree permanently.
if ! printf 'include(${CMAKE_CURRENT_SOURCE_DIR}/audio_deps.cmake)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 no longer matches include() -- the flagship bypass is open:" >&2
  echo "         an included file is read by no part of prong A." >&2
  exit 2
fi
# A literal in which ONLY the trailing [[:space:]]*\( is load-bearing: there is no
# target_include_directories( here for the leading bracket to be tested by, so this arm can be the
# one that fires. (Its first form reused the target_include_directories shape from the arm above,
# which meant both arms matched under both mutations and this one could never fire at all -- dead
# code that also mis-attributed the cause. 3.7.3's second code-review round, finding 3.)
if printf 'set(AERO_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)\n' \
     | strip_cmake | grep -qiE "$BANNED_CMAKE_RE"; then
  echo "::error::BANNED_CMAKE_RE in $0 over-matches a path ending in /include -- the trailing" >&2
  echo "         [[:space:]]*\\( has been lost and this guard would red the tree it ships into." >&2
  exit 2
fi
# NOTE ON SELF-TESTS THAT ARE GONE, because a removed check should say so. Arms pinning
# prop_targets() were retired with the helper: Part 1d no longer asks WHICH command names a guarded
# target, only whether anything does, so there is no per-command extractor left to rot. The property
# spellings are covered by stages S2e/S6d and by the name sweep itself.

# The three arms below pin Part 1d's ACTUAL predicate, using the same regex the sweep uses.
#
# They replace four arms that pinned tll_target()/is_vcpkg_free_target() while claiming to use "the
# same helpers the sweep uses so the self-test cannot drift" -- a claim that had already gone false:
# Part 1d stopped calling either when it became a name sweep, so those helpers were reachable only
# from their own self-tests, and the near-miss they were protecting was pinned by nothing. Deleting
# the trailing boundary left all four passing. A self-test that outlives the code it describes is
# worse than no self-test, because it reads as coverage.
if ! printf 'target_link_libraries(aero_audio PRIVATE miniaudio)\n' \
     | grep -qE "$(target_mention_re 'aero_audio')"; then
  echo "::error::target_mention_re in $0 no longer matches a real mention of a guarded target --" >&2
  echo "         Part 1d's sweep is vacuous." >&2
  exit 2
fi
if ! printf '    aero_audio\n' | grep -qE "$(target_mention_re 'aero_audio')"; then
  echo "::error::target_mention_re in $0 misses a target on its own line -- a wrapped call would" >&2
  echo "         pass the cross-directory sweep." >&2
  exit 2
fi
if printf 'target_link_libraries(aero_audio_boundary_probe PRIVATE aero::audio)\n' \
     | grep -qE "$(target_mention_re 'aero_audio')"; then
  echo "::error::target_mention_re in $0 matches aero_audio_boundary_probe -- the trailing name" >&2
  echo "         boundary has been lost, and this guard would red the tree it ships into." >&2
  exit 2
fi
if ! printf '#include <miniaudio.h>\n' | grep -qE "$MA_INCLUDE_RE"; then
  echo "::error::MA_INCLUDE_RE in $0 no longer matches a real miniaudio include -- vacuous." >&2
  exit 2
fi
if ! printf 'ma_device* leak;\n' | grep -qE "$MA_IDENTIFIER_RE"; then
  echo "::error::MA_IDENTIFIER_RE in $0 no longer matches a real miniaudio identifier -- vacuous." >&2
  exit 2
fi
if printf '// wraps a ma_device\n' | sed 's|//.*||' | grep -qE "$MA_IDENTIFIER_RE"; then
  echo "::error::Comment-stripping in $0 is broken -- a pure // comment line still matches." >&2
  exit 2
fi

violations=""
DIR_SCOPE_FILES="$(dir_scope_files)"
readonly DIR_SCOPE_FILES
# Two statements, not `readonly X="$(...)"`: the combined form makes the assignment's exit status
# the BUILTIN's, so a failing dir_scope_files would be masked entirely. And an anti-vacuity assert,
# because an empty or truncated scope set silently disables the whole directory-scope half while
# every other arm still passes -- the sibling guard caught exactly that during its own bring-up,
# when a deleted variable left the set empty and the guard still exited 0.
for _need in 'CMakeLists.txt' 'engine/CMakeLists.txt'; do
  if ! printf '%s\n' "$DIR_SCOPE_FILES" | grep -qxF "$_need"; then
    echo "::error::audio-boundary guard: the directory-scope set is missing '${_need}', so nothing" >&2
    echo "         checks a directory-scoped command written there. Fix dir_scope_files in $0." >&2
    exit 2
  fi
done

# --- Part 1a: no vcpkg hook command in the three CMakeLists. -----------------------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  hits="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -iE "$BANNED_CMAKE_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${f}:${n}: a vcpkg/dependency hook command entered a vcpkg-free CMakeLists
"
    done <<< "$hits"
  fi
done

# --- Part 1b: every link token in the three CMakeLists is an aero:: engine target. -------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  calls="$(extract_calls 'target_link_libraries' < "$f")"
  [ -z "$calls" ] && continue
  while IFS= read -r call; do
    tokens="$(printf '%s\n' "$call" | sed -E 's/^[^(]*\(//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d')"
    while IFS= read -r tok; do
      # Both spellings, via one glob: the aero::…_internal alias AND the raw aero_…_internal target.
      # Every one of aero_scene_internal / aero_platform_internal / aero_rhi_internal exists in this
      # tree and matches LINK_TOKEN_RE's aero_[a-z_]+ branch, so an alias-only check waved them
      # through -- and the raw name is the spelling anyone copying from engine/scene/CMakeLists.txt
      # would write.
      if case "$tok" in *_internal) true ;; *) false ;; esac; then
        line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F -- "$tok" | head -1 | cut -d: -f1 || true)"
        violations="${violations}${f}:${line:-1}: '${tok}' on a link line -- an *_internal target carries its backend INTERFACE by design
"
      elif ! printf '%s\n' "$tok" | grep -qE "$LINK_TOKEN_RE"; then
        line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F -- "$tok" | head -1 | cut -d: -f1 || true)"
        violations="${violations}${f}:${line:-1}: link token '${tok}' is not an aero:: engine target -- this CMakeLists is vcpkg-free by contract
"
      fi
    done <<< "$tokens"
  done <<< "$calls"
done

# --- Part 1c: include directories stay inside the subsystem. -----------------------------------
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  calls="$(extract_calls 'target_include_directories' < "$f")"
  [ -z "$calls" ] && continue
  while IFS= read -r call; do
    tokens="$(printf '%s\n' "$call" | sed -E 's/^[^(]*\(//; s/\)$//' | tr ' \t' '\n\n' | sed '/^$/d')"
    while IFS= read -r tok; do
      case "$tok" in
        # SYSTEM / BEFORE / AFTER are legal target_include_directories KEYWORDS, not paths. Without
        # them a wholly legal `target_include_directories(aero_audio SYSTEM PUBLIC
        # ${CMAKE_CURRENT_SOURCE_DIR}/include)` exits 1 with a message calling SYSTEM an include
        # path -- and, worse, any seed containing SYSTEM produced a violation for a reason that had
        # nothing to do with the path being tested, which is how this arm's e2e stage came to pass
        # while the arm itself was mutated open (3.7.3's code-review round, finding 2).
        aero_*|PUBLIC|PRIVATE|INTERFACE|SYSTEM|BEFORE|AFTER) ;;
        '${CMAKE_CURRENT_SOURCE_DIR}'*) ;;
        *)
          line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' | grep -F -- "$tok" | head -1 | cut -d: -f1 || true)"
          violations="${violations}${f}:${line:-1}: include dir '${tok}' reaches outside the subsystem -- only \${CMAKE_CURRENT_SOURCE_DIR}-rooted paths are legal here
"
          ;;
      esac
    done <<< "$tokens"
  done <<< "$calls"
done

# --- Part 1e: a guarded CMakeLists may contain NOTHING BUT the three allowed commands. -----------
# The allowlist half. Parts 1a/1b/1c stay because their messages name the specific vector, which is
# what a reader needs; this arm is the completeness net behind them, and it is what makes a command
# nobody has thought of -- set_source_files_properties(src/clip.cpp PROPERTIES INCLUDE_DIRECTORIES
# ...), say, which reaches an external header without naming the target at all -- a violation on
# arrival instead of a fourth review round.
for f in "${VCPKG_FREE_CMAKE[@]}"; do
  [ -f "$f" ] || continue
  calls="$(all_calls < "$f")"
  [ -z "$calls" ] && continue
  while IFS= read -r call; do
    [ -z "$call" ] && continue
    cmd="$(printf '%s\n' "$call" | call_name)"
    case " ${GUARDED_FILE_COMMANDS} " in
      *" ${cmd} "*) continue ;;
    esac
    line="$(nl -ba -w1 -s: "$f" | sed 's|#.*||' \
            | grep -iE "(^|[^a-zA-Z0-9_])${cmd}[[:space:]]*\\(" | head -1 | cut -d: -f1 || true)"
    violations="${violations}${f}:${line:-1}: '${cmd}' is not one of the three commands a vcpkg-free CMakeLists may use (add_library, target_include_directories, target_link_libraries). Some rejections here are harmless in themselves -- target_sources is the modern idiom for adding a source file and touches no dependency -- but the allowlist is what makes this guard closed rather than a list of banned commands. Widening it is a deliberate edit to GUARDED_FILE_COMMANDS in $0, with a comment saying why
"
  done <<< "$calls"
done

# --- Part 1d: NO OTHER CMake FILE MAY NAME A GUARDED TARGET AT ALL. -------------------------------
# The other allowlist half, and the reason it is stated as "at all" rather than as a list of
# commands: there are ZERO legitimate references to aero_assets / aero_audio / aero_scene_audio
# outside their own CMakeLists in this tree, so the complete predicate is simply "none". That closes
# target_link_libraries, set_target_properties, set_property, target_include_directories,
# target_compile_options, target_compile_definitions, a bare if(TARGET ...), a variable assignment,
# and every spelling not yet invented -- with nothing to enumerate and nothing to keep up to date.
# CMake >= 3.13 is what makes the cross-directory forms reachable in the first place.
swept=0
while IFS= read -r -d '' f; do
  if is_guarded_file "$f"; then continue; fi
  # A file can be TRACKED and ABSENT -- `git add` then `rm`, the ordinary middle of a rename. Reading
  # it by name would abort the whole guard with a shell error and no verdict at all.
  [ -f "$f" ] || continue
  swept=$((swept + 1))
  numbered="$(nl -ba -w1 -s: "$f" | sed 's|#.*||')"
  # A file in the directory-scope set may not open a directory-scoped hole above the guarded targets.
  if printf '%s\n' "$DIR_SCOPE_FILES" | grep -qxF "$f"; then
    # FLATTENED, like every other arm here -- a line-scoped keyword grep misses a wrapped call.
    dhit="$(flat_file_text "$f" | grep -oiE "$DIR_SCOPED_RE" | head -1 || true)"
    if [ -n "$dhit" ]; then
      _cmd="$(printf '%s\n' "$dhit" | sed -E 's/^[^a-zA-Z_]*//; s/[[:space:]]*\(.*$//' | tr 'A-Z' 'a-z')"
      line="$(printf '%s\n' "$numbered" | grep -iE "(^|[^a-zA-Z0-9_])${_cmd}[[:space:]]*\(" \
              | head -1 | cut -d: -f1 || true)"
      violations="${violations}${f}:${line:-1}: '${_cmd}' here reaches a vcpkg-free target without naming it -- directory properties are inherited by everything below
"
    fi
  fi
  for tgt in $VCPKG_FREE_TARGETS; do
    hits="$(printf '%s\n' "$numbered" | grep -E "$(target_mention_re "$tgt")" || true)"
    [ -z "$hits" ] && continue
    while IFS= read -r hit; do
      [ -z "$hit" ] && continue
      n="${hit%%:*}"
      violations="${violations}${f}:${n}: '${tgt}' is named outside its own CMakeLists -- nothing else in the tree may reference a vcpkg-free target. This is intentionally strict: a message(), an install(TARGETS ...) or a foreach() naming one of the three is refused too. If such a reference is genuinely wanted, that is a deliberate widening -- add the exception to $0 with a comment saying why, exactly as widening ALLOWED_FILE is treated elsewhere
"
    done <<< "$hits"
  done
done < <(git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake')

# Anti-vacuity: a sweep that walked nothing proves nothing, and would print its OK banner all the
# same. This tree always has other CMake files; zero means the pathspec or the roster has rotted.
if [ "$swept" -eq 0 ]; then
  echo "::error::audio-boundary guard: the cross-directory sweep walked ZERO other CMake files --" >&2
  echo "         its pathspec or the guarded-file roster has rotted, so Part 1d proves nothing." >&2
  exit 2
fi

# --- Part 2: no miniaudio token anywhere under the audio roots (sources included). -------------
scanned=0
while IFS= read -r -d '' file; do
  case "$file" in
    *.cpp|*.hpp|*.h|*.c|*.cc|*.cxx|*.hxx|*.inl|*.mm|*.m) ;;
    *) continue ;;
  esac
  # Same tracked-but-absent case as Part 1d: `nl` on a missing path would abort the guard.
  [ -f "$file" ] || continue
  scanned=$((scanned + 1))
  stripped="$(nl -ba -w1 -s: "$file" | sed -E 's|//.*||')"
  hits="$(printf '%s\n' "$stripped" | grep -E "$MA_IDENTIFIER_RE" || true)"
  if [ -n "$hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: miniaudio identifier used as code inside the audio layer
"
    done <<< "$hits"
  fi
  inc_hits="$(grep -nE "$MA_INCLUDE_RE" "$file" || true)"
  if [ -n "$inc_hits" ]; then
    while IFS= read -r hit; do
      n="${hit%%:*}"
      violations="${violations}${file}:${n}: miniaudio header included inside the audio layer
"
    done <<< "$inc_hits"
  fi
done < <(git ls-files -z -- "${AUDIO_ROOTS[@]}")

if [ -n "$violations" ]; then
  echo "audio boundary broken -- task 3.7.3 / ADR-006 / docs/04 guard table / epic 3.7 DoD:" >&2
  echo "$violations" >&2
  echo "" >&2
  echo "engine/audio and engine/scene_audio are first-party layers over engine types ONLY, and" >&2
  echo "aero_assets/aero_audio/aero_scene_audio link no vcpkg package -- that is what makes their" >&2
  echo "PRIVATE links a real compile-time boundary (R12). A device belongs behind" >&2
  echo "platform::AudioDevice (engine/platform); a decoder belongs in editor/src/audio_decode.cpp." >&2
  if [ -n "${GITHUB_ACTIONS:-}" ]; then
    while IFS= read -r v; do
      [ -z "$v" ] && continue
      f="${v%%:*}"; rest="${v#*:}"; n="${rest%%:*}"
      echo "::error file=${f},line=${n}::audio boundary broken (task 3.7.3; ADR-006). Keep engine/audio + engine/scene_audio miniaudio-free and their CMakeLists vcpkg-free; route devices through platform::AudioDevice and decodes through editor/src/audio_decode.cpp."
    done <<< "$violations"
  fi
  exit 1
fi

echo "audio-boundary guard: OK -- ${scanned} tracked engine/audio+engine/scene_audio sources scanned; ${#VCPKG_FREE_CMAKE[@]} vcpkg-free CMakeLists verified; ${swept} other CMake files swept for cross-directory links; miniaudio confined to engine/platform/src/{audio_device.cpp, miniaudio_impl.c} + editor/src/audio_decode.cpp"

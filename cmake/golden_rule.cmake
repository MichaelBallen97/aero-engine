# cmake/golden_rule.cmake — task 2.1.2 (project rule #1, docs/00 / docs/03 "The golden rule"; docs/04
# guard table). This is the LINK-graph and INCLUDE-DIRECTORY half of the golden-rule guard; the
# TEXTUAL half is .github/scripts/check-golden-rule.sh. Neither subsumes the other: an engine target
# that links an editor target, or that carries an editor include directory, compiles cleanly and is
# invisible to any #include scan (see the header comment of the script for the full four-row table of
# ways the rule can be broken).
#
# Defines FOUR functions. Only the last is public; nothing is executed at include() time.
#
#     _aero_gr_collect_targets(<dir> <out-var>)     # BUILDSYSTEM_TARGETS + recurse SUBDIRECTORIES
#     _aero_gr_resolve_entry(<entry> <out-var>)      # tokenise a raw link entry, keep+de-alias TARGETs
#     _aero_gr_closure(<root-target> <out-var>)      # BFS over LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES
#     aero_assert_golden_rule(CONSUMER_DIRS <d>... FORBIDDEN_DIRS <d>...
#                             [CANARY_CONSUMER <t>] [CANARY_DEP <t>])
#
# aero_assert_golden_rule() FATAL_ERRORs the configure if any target rooted under CONSUMER_DIRS
# transitively links, or carries on its include path, any target/directory rooted under
# FORBIDDEN_DIRS. The optional CANARY_CONSUMER/CANARY_DEP pair is a REVERSE canary: it asserts that
# CANARY_CONSUMER transitively reaches CANARY_DEP, which proves the walk actually TRAVERSES rather
# than silently passing because it found nothing (there is no allowlisted-file canary here, unlike
# the script — nothing under engine/ or runtime/ may ever reference the editor).
#
# NOTE for 5.2.2 (the runtime-purity guard, the fifth and last architecture guard): reuse
# _aero_gr_closure() rather than writing a second link-graph walk — "does the runtime binary
# transitively link ImGui/Assimp/libclang" is the identical query with a different forbidden set.
# _aero_gr_closure() is written to be golden-rule-agnostic on purpose: it takes an arbitrary root and
# returns its transitive link closure, and contains no mention of "editor" or any golden-rule
# vocabulary. Keep it that way.

# _aero_gr_collect_targets(<dir> <out-var>) — real (never IMPORTED/ALIAS) target names declared in
# <dir>, recursively through every add_subdirectory()'d child (F6). BUILDSYSTEM_TARGETS is documented
# to exclude Imported/Alias targets but INCLUDE Interface Libraries — load-bearing here, since the
# probe project (§C2) and several real engine targets (aero_platform_internal, aero_rhi_internal,
# aero_scene_internal) are INTERFACE libraries.
#
# get_property(DIRECTORY) requires <dir> to already have been add_subdirectory()'d. If <dir> was
# never added at all (a typo), return an EMPTY set rather than erroring here — that lets the caller's
# own anti-vacuity check (step 2 below) report the pinned "cannot self-verify" wording instead of a
# raw, internal-looking CMake diagnostic. If <dir> exists on disk but was genuinely never
# add_subdirectory()'d, get_property() itself raises a CMake error naming the directory, which is loud
# enough on its own.
function(_aero_gr_collect_targets dir out_var)
    if(NOT IS_DIRECTORY "${dir}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(result "")
    get_property(_gr_here DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    if(_gr_here)
        list(APPEND result ${_gr_here})
    endif()

    get_property(_gr_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_gr_sub IN LISTS _gr_subdirs)
        _aero_gr_collect_targets("${_gr_sub}" _gr_child)
        if(_gr_child)
            list(APPEND result ${_gr_child})
        endif()
    endforeach()

    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# _aero_gr_resolve_entry(<entry> <out-var>) — a raw LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES entry
# arrives UNEXPANDED, with generator expressions intact (F9): aero_core links aero::profiling PRIVATE,
# which materialises on the interface as `$<LINK_ONLY:aero::profiling>` — a string, not a target name.
# Extract identifier-ish tokens and ask if(TARGET ...) of each rather than parsing generator-expression
# grammar; tokens that are not targets (raw library names like `m`, `-framework CoreAudio` fragments,
# bare paths) are silently dropped — only targets can be under a forbidden/consumer tree. De-alias via
# ALIASED_TARGET (documented-safe on an alias target; NOTFOUND on a non-alias, which if() treats as
# false, so the non-alias branch is the normal path).
function(_aero_gr_resolve_entry entry out_var)
    set(result "")
    string(REGEX MATCHALL "[A-Za-z0-9_:+.-]+" _gr_toks "${entry}")
    foreach(_gr_tok IN LISTS _gr_toks)
        if(TARGET ${_gr_tok})
            get_target_property(_gr_alias ${_gr_tok} ALIASED_TARGET)
            if(_gr_alias)
                list(APPEND result "${_gr_alias}")
            else()
                list(APPEND result "${_gr_tok}")
            endif()
        endif()
    endforeach()
    if(result)
        list(REMOVE_DUPLICATES result)
    endif()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# _aero_gr_closure(<root-target> <out-var>) — the transitive link closure of <root-target> (never
# including <root-target> itself), via an explicit worklist BFS. Skips LINK_LIBRARIES on IMPORTED
# targets (documented as never meaningfully set there — F8) and tolerates NOTFOUND everywhere (reading
# LINK_LIBRARIES on an INTERFACE library is documented-safe since CMake 3.19; the project floor is
# 3.28). Every worklist MEMBER is guaranteed to already be a real target, because
# _aero_gr_resolve_entry() only ever emits names for which if(TARGET ...) held — so the ONLY way a
# non-target can reach this function is via <root-target> itself, which is why that is checked
# explicitly below.
#
# Precondition: <root-target> MUST be a TARGET. get_target_property() on a non-existent target is a
# HARD CMake error ("called with non-existent target"), not a NOTFOUND — so this FATAL_ERRORs with a
# deliberately GENERIC, golden-rule-free message (an internal precondition violation is a caller bug).
# The user-facing translation into the pinned "cannot self-verify" wording belongs one layer up, in
# aero_assert_golden_rule()'s own pre-check (step 3a) — that pre-check is what actually stops a caller
# from ever reaching this branch in practice; this FATAL_ERROR is the second, defensive layer.
function(_aero_gr_closure root out_var)
    if(NOT TARGET "${root}")
        message(FATAL_ERROR "_aero_gr_closure: internal precondition violation -- '${root}' is not a CMake target. (This function must only ever be called with a caller-verified target.)")
    endif()

    set(worklist "${root}")
    set(seen "")
    while(worklist)
        list(POP_FRONT worklist _gr_t)
        if(_gr_t IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${_gr_t}")

        set(_gr_entries "")
        get_target_property(_gr_imported ${_gr_t} IMPORTED)
        if(NOT _gr_imported)
            get_target_property(_gr_v ${_gr_t} LINK_LIBRARIES)
            if(_gr_v)
                list(APPEND _gr_entries ${_gr_v})
            endif()
        endif()
        get_target_property(_gr_w ${_gr_t} INTERFACE_LINK_LIBRARIES)
        if(_gr_w)
            list(APPEND _gr_entries ${_gr_w})
        endif()

        foreach(_gr_e IN LISTS _gr_entries)
            _aero_gr_resolve_entry("${_gr_e}" _gr_r)
            if(_gr_r)
                list(APPEND worklist ${_gr_r})
            endif()
        endforeach()
    endwhile()

    list(REMOVE_ITEM seen "${root}")
    set(${out_var} "${seen}" PARENT_SCOPE)
endfunction()

# aero_assert_golden_rule(CONSUMER_DIRS <d>... FORBIDDEN_DIRS <d>...
#                         [CANARY_CONSUMER <t>] [CANARY_DEP <t>]) — the public entry point. See the
# module header comment above and docs/04's guard table for the invariant. Six steps:
#
#   1. Collect the consumer set (union over CONSUMER_DIRS) and the forbidden set (union over
#      FORBIDDEN_DIRS), each recursively via _aero_gr_collect_targets(), deduped.
#   2. Anti-vacuity: FATAL_ERROR if EITHER set is empty, on the COMBINED set — never per-directory
#      (runtime/ has zero targets today by design; a per-directory check would fail on a correct
#      tree — spec §5's "single most likely implementation bug in the task"). Never a hardcoded count
#      or roster either: AERO_REFLECT_TOOLS=OFF legitimately drops a target from the consumer set.
#   3. Reverse canary (only if CANARY_CONSUMER/CANARY_DEP given — both or neither):
#        3a. both must satisfy if(TARGET ...), else "cannot self-verify" / "named canary target does
#            not exist" — this is what stops a caller from ever handing _aero_gr_closure() a
#            non-existent root (that would be a hard CMake error instead of this pinned wording).
#        3b. CANARY_DEP must be in the transitive closure of CANARY_CONSUMER, else "cannot
#            self-verify" / "reverse canary" — proves the walk TRAVERSES rather than passing because
#            it found nothing.
#   4. Link check: for each consumer, any forbidden target in its closure is a finding.
#   5. Include-directory check (docs/04; this generated-source hole is why this half exists at all —
#      git ls-files sees tracked files only, and this project generates .json.gen.cpp/.meta.gen.cpp
#      into the build tree with a production consumer since task 1.4.2): for each consumer, every
#      entry of INCLUDE_DIRECTORIES + INTERFACE_INCLUDE_DIRECTORIES that resolves (via
#      get_filename_component(... ABSOLUTE) — never REALPATH, which requires existence) inside a
#      resolved FORBIDDEN_DIRS entry (via file(RELATIVE_PATH), never a substring match on the literal
#      word "editor" — a repo checked out under a directory literally named `editor` must not
#      false-positive) is a finding. Entries beginning with a generator expression are skipped
#      (documented: such a path is stored unmodified, and none exist in the tree today).
#   6. Report: accumulate every finding and emit ONE terminal FATAL_ERROR (never SEND_ERROR, which
#      would let generation proceed far enough to produce confusing secondary failures) with the
#      remediation paragraph. If clean, a STATUS one-liner naming both set sizes.
function(aero_assert_golden_rule)
    cmake_parse_arguments(ARG "" "CANARY_CONSUMER;CANARY_DEP" "CONSUMER_DIRS;FORBIDDEN_DIRS" ${ARGN})

    set(_gr_remediation "The dependency must point the other way: move the type into engine/, or invert the call with a callback/observer seam -- see aero::platform_internal's RawEventAccessor (engine/platform/internal/aero/platform/internal/native_event.hpp) for the sanctioned shape.")

    # --- Step 1: collect. ---------------------------------------------------------------------
    set(consumers "")
    foreach(_gr_d IN LISTS ARG_CONSUMER_DIRS)
        _aero_gr_collect_targets("${_gr_d}" _gr_t)
        if(_gr_t)
            list(APPEND consumers ${_gr_t})
        endif()
    endforeach()
    if(consumers)
        list(REMOVE_DUPLICATES consumers)
    endif()

    set(forbidden "")
    foreach(_gr_d IN LISTS ARG_FORBIDDEN_DIRS)
        _aero_gr_collect_targets("${_gr_d}" _gr_t)
        if(_gr_t)
            list(APPEND forbidden ${_gr_t})
        endif()
    endforeach()
    if(forbidden)
        list(REMOVE_DUPLICATES forbidden)
    endif()

    # --- Step 2: anti-vacuity, on the COMBINED set, never per-directory. -----------------------
    if(NOT consumers)
        message(FATAL_ERROR "golden-rule assertion cannot self-verify: no targets under CONSUMER_DIRS (${ARG_CONSUMER_DIRS}). The guard refuses to pass rather than silently checking nothing.")
    endif()
    if(NOT forbidden)
        message(FATAL_ERROR "golden-rule assertion cannot self-verify: no targets under FORBIDDEN_DIRS (${ARG_FORBIDDEN_DIRS}). The guard refuses to pass rather than silently checking nothing.")
    endif()

    # --- Step 3: reverse canary (optional, both-or-neither). -----------------------------------
    if(DEFINED ARG_CANARY_CONSUMER OR DEFINED ARG_CANARY_DEP)
        if(NOT DEFINED ARG_CANARY_CONSUMER OR NOT DEFINED ARG_CANARY_DEP)
            message(FATAL_ERROR "golden-rule assertion cannot self-verify: CANARY_CONSUMER and CANARY_DEP must both be given, or neither (caller bug).")
        endif()
        # 3a — existence pre-check: a named-but-absent canary must be a pinned "cannot self-verify"
        # failure, never a raw CMake "non-existent target" error out of _aero_gr_closure().
        if(NOT TARGET "${ARG_CANARY_CONSUMER}" OR NOT TARGET "${ARG_CANARY_DEP}")
            message(FATAL_ERROR "golden-rule assertion cannot self-verify: named canary target does not exist (CANARY_CONSUMER=${ARG_CANARY_CONSUMER}, CANARY_DEP=${ARG_CANARY_DEP}).")
        endif()
        # 3b — containment: proves the walk actually traverses.
        _aero_gr_closure("${ARG_CANARY_CONSUMER}" _gr_canary_closure)
        if(NOT "${ARG_CANARY_DEP}" IN_LIST _gr_canary_closure)
            message(FATAL_ERROR "golden-rule assertion cannot self-verify: reverse canary -- '${ARG_CANARY_CONSUMER}' does not transitively link '${ARG_CANARY_DEP}'. Either the canary pair is wrong, or the closure walk itself is broken.")
        endif()
    endif()

    # --- Steps 4 + 5: link check and include-directory check. ----------------------------------
    set(_gr_findings "")
    foreach(_gr_c IN LISTS consumers)
        _aero_gr_closure("${_gr_c}" _gr_closure)
        foreach(_gr_f IN LISTS forbidden)
            if(_gr_f IN_LIST _gr_closure)
                string(APPEND _gr_findings "  ${_gr_c} links ${_gr_f}\n")
            endif()
        endforeach()

        set(_gr_dirs "")
        get_target_property(_gr_id ${_gr_c} INCLUDE_DIRECTORIES)
        if(_gr_id)
            list(APPEND _gr_dirs ${_gr_id})
        endif()
        get_target_property(_gr_iid ${_gr_c} INTERFACE_INCLUDE_DIRECTORIES)
        if(_gr_iid)
            list(APPEND _gr_dirs ${_gr_iid})
        endif()

        foreach(_gr_entry IN LISTS _gr_dirs)
            if(_gr_entry MATCHES "\\$<")
                continue()   # a path beginning with a generator expression is stored unmodified (documented); none exist today.
            endif()
            get_filename_component(_gr_entry_abs "${_gr_entry}" ABSOLUTE)
            foreach(_gr_fd IN LISTS ARG_FORBIDDEN_DIRS)
                get_filename_component(_gr_fd_abs "${_gr_fd}" ABSOLUTE)
                file(RELATIVE_PATH _gr_rel "${_gr_fd_abs}" "${_gr_entry_abs}")
                if(NOT _gr_rel MATCHES "^\\.\\.($|/)")
                    string(APPEND _gr_findings "  ${_gr_c} has ${_gr_entry_abs} on its include path\n")
                endif()
            endforeach()
        endforeach()
    endforeach()

    # --- Step 6: report. -------------------------------------------------------------------
    if(_gr_findings)
        message(FATAL_ERROR "GOLDEN RULE VIOLATION -- the engine must never depend on the editor (project rule #1; docs/00, docs/03; task 2.1.2):\n${_gr_findings}\n${_gr_remediation}")
    endif()

    list(LENGTH consumers _gr_n_consumers)
    list(LENGTH forbidden _gr_n_forbidden)
    message(STATUS "golden rule: OK -- ${_gr_n_consumers} engine/runtime targets checked against ${_gr_n_forbidden} editor targets")
endfunction()

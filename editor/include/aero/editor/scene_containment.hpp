#pragma once
// Aero Engine -- is this scene inside the open project? (task E.4.2, D1-D4). PUBLIC, and free of
// ImGui, SDL, entt and <filesystem> -- the project.hpp / scene_session.hpp shape, so the PURE half
// below is reachable from the UNGATED tier-0 aero_editor_shell_test with no window and no GPU. Held
// by FILE PLACEMENT (R12), like every other header under editor/include.
//
// THIS HEADER AND scene_containment.cpp ARE FREE OF EVERY BUILD GATE. Containment reads no scene and
// serializes nothing, so it never touches the engine's serialization bridge -- which is why
// tests/editor/scene_containment_test.cpp is present and passing in ALL THREE build configurations,
// unlike scene_io_test.cpp, which is ABSENT from the reflect-OFF build by design.
//
// NOTHING HERE LOGS (the project.hpp / project_files.hpp convention, applied a fourth time). The
// verdict and its reason are RETURNED; the CALLER logs exactly one ERROR (D6).
//
// THE SAFETY PROPERTY, stated once: every layer is built so that a mistake produces a false REFUSAL,
// never a false ACCEPT (D3). An un-normalised path is Unresolvable rather than compared; the
// canonical rescue only ever WIDENS a refusal and never narrows an acceptance.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

// D8's bound on the upward walk. Exceeding it means NO OFFER is made -- the refusal still happens,
// with the "outside the open project" wording. A degradation, never a wrong answer.
inline constexpr std::size_t MAX_PROJECT_SEARCH_DEPTH = 64;

// EVERY enum carries an explicit underlying type: performance-enum-size is --warnings-as-errors on
// the Linux lane (the scene_session.hpp rule).
enum class SceneContainment : std::uint8_t {
    Contained = 0,  // PERMITTED -- the scene's directory is the project root or lies under it
    NoProject,      // PERMITTED -- no project is open, so there is nothing to be outside of (D5)
    Outside,        // REFUSED
    Unresolvable,   // REFUSED -- an empty path, or one still carrying a ".." segment (D3)
};

// The ONE place "may this proceed?" is decided. Never spelled as a comparison at a call site: a
// second copy is how a fourth enumerator gets added and one of the two readers keeps permitting it.
[[nodiscard]] constexpr bool containmentPermits(SceneContainment state) noexcept {
    return state == SceneContainment::Contained || state == SceneContainment::NoProject;
}

// ---- the PURE core (no I/O, no <filesystem>, every one a tier-0 case) --------------------------

// TRUE iff `directoryUtf8` IS `rootUtf8` or lies strictly under it, compared SEGMENT-WISE (D2): the
// leading separator run must match in LENGTH (0 relative, 1 POSIX-absolute, 2 UNC) and `root`'s
// segments must be a prefix of `directory`'s. '/' and '\\' are both separators; interior and
// trailing empties and "." segments are dropped; a ".." segment on EITHER side, ANYWHERE, returns
// false (D3) -- including past the end of the root's own segments, because this function is PUBLIC
// and a caller that did not normalise first must get a refusal rather than a wrong accept.
// Comparison is BYTE-EXACT and never case-folded -- the canonical rescue is what answers the case
// and drive-letter questions (D4/F8), and folding here would accept a foreign scene on a
// case-SENSITIVE volume.
//
// An EMPTY `rootUtf8` returns false: "is it under nothing" is not "yes". The NoProject decision
// belongs to the resolver, which tests it before it ever gets here.
//
// noexcept AND ALLOCATION-FREE (task E.4.2 U1): a two-cursor walk, no container, no std::string.
// It runs on every open and every save, and a bad_alloc inside a noexcept function is
// std::terminate -- project.hpp:106-108 states that rule pointed the other way, at assetsRoot().
[[nodiscard]] bool directoryWithin(std::string_view directoryUtf8, std::string_view rootUtf8) noexcept;

// The FILE-shaped face of the same rule: the scene's PARENT directory must satisfy directoryWithin.
// Returns NoProject for an empty root, Unresolvable for an empty scene path or a surviving "..",
// Contained/Outside otherwise. A scene path that IS the root exactly is Outside -- a root is a
// directory and cannot be a document, and that falls out of taking directoryOf() first rather than
// needing a separate check.
[[nodiscard]] SceneContainment lexicalContainment(std::string_view scenePathUtf8,
                                                  std::string_view projectRootUtf8) noexcept;

// ---- the resolved verdict (scene_containment.cpp; ALL <filesystem>) -----------------------------

struct ContainmentVerdict {
    SceneContainment state = SceneContainment::NoProject;
    // D8, and ONLY for an Outside verdict on an OPEN. "" when no enclosing project.json was found,
    // when the walk hit MAX_PROJECT_SEARCH_DEPTH, or when the refusal was a SAVE (D9 -- the walk is
    // not even performed).
    std::string owningProjectRoot;
    // The manifest's `name`. "" when the project.json is present but unreadable or unparseable (D8),
    // in which case `owningProjectRoot` is still set and the offer is still made.
    std::string owningProjectName;
};

// PURE COMPUTE, NO DISK: std::filesystem::absolute (which consults only the process CWD) +
// lexically_normal + separators unified to '/' + trailing separator stripped. "" in, "" out -- the
// projectRootFromPath contract, and the P84 lesson verbatim: absolute("") is NOT empty, so emptiness
// is tested BEFORE absolutization or "no path given" silently becomes "the working directory".
// Defined in the .cpp because unifying separators and resolving ".." without a platform conditional
// (forbidden, 2.6.1's D4/AC-35) is exactly what path's lexical machinery does for free, and THIS
// header must stay <filesystem>-free.
[[nodiscard]] std::string normalizeForContainment(std::string_view pathUtf8);

// THE function the two choke points call. LEXICAL FIRST -- zero filesystem calls on the permitted
// path (D4/AC-9). The canonical rescue runs ONLY on a lexical refusal and can only ever turn Outside
// INTO Contained. `findOwningProject` performs D8's upward walk; it is FALSE for a save (D9).
[[nodiscard]] ContainmentVerdict resolveSceneContainment(std::string_view scenePathUtf8,
                                                         std::string_view projectRootUtf8, bool findOwningProject);

// ---- the ONE sentence (D6) ----------------------------------------------------------------------
// The reason clause both the Console ERROR and the modal hand to the user VERBATIM, so the wording
// cannot drift between them (the BLEND_UNCONVERTED_MESSAGE precedent,
// editor/src/scene_asset_loader.hpp:42-46). Every non-empty result ENDS IN A PERIOD -- the modal
// draws it as a sentence and the ERROR reads as one; putting the period in one consumer and not the
// other is exactly the drift this function exists to prevent. "" iff
// containmentPermits(verdict.state) -- a permitted verdict has no reason to give.
[[nodiscard]] std::string containmentReason(const ContainmentVerdict& verdict, std::string_view projectRootUtf8,
                                            bool forSave);

}  // namespace engine::editor

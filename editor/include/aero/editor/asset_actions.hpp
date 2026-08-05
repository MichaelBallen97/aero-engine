#pragma once
// Aero Engine — the orphan-sidecar delete action (task 3.1.3). PUBLIC, and the asset_meta.hpp shape
// verbatim: free of ImGui, SDL, entt and every build gate. The pure half is a PLANNER (asset_cache.hpp
// D17's shape, 3.1.2's precedent): the policy is a function of values, and the ONE `.cpp` call site is
// glue plus exactly one std::filesystem::remove.
//
// This is the FIRST destructive path in the editor (R5): every check below is re-verified from disk,
// in a fixed order, and any doubt refuses rather than guesses.
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

enum class OrphanDeleteRefusal : std::uint8_t {
    None = 0,
    NotAMetaName,  // the path does not end in .meta (isMetaFileName on the LEAF)
    EscapesRoot,   // an absolute path, a rooted drive letter, a ".." segment, or a backslash
    Missing,       // the file is gone -- someone else deleted it. Nothing to do; not an error
    NotAMeta,      // it exists but does not parse as a .meta v1 (D12 check 4)
    AssetPresent,  // the asset it names exists again -- it is NOT an orphan any more (E22)
    RemoveFailed,  // the OS refused
};

struct OrphanDeleteResult {
    bool deleted = false;
    OrphanDeleteRefusal refusal = OrphanDeleteRefusal::None;
    std::string message;  // the exact human text the CALLER logs; "" iff deleted
};

// PURE (no disk): the path-shape half of D12's checks, so every branch is a tier-0 case.
// Rejects: a leaf that is not a sidecar name; an EMPTY path; an absolute path ('/' or "X:" prefix);
// ANY path containing a ".." segment; ANY backslash -- a Windows separator must never reach a
// relative key, because 2.2.4's relative paths are '/'-separated on every OS.
[[nodiscard]] OrphanDeleteRefusal validateOrphanPath(std::string_view relativeMetaPath) noexcept;

// THE ACTION. Re-verifies EVERYTHING from disk, in a fixed order, then removes exactly ONE file.
// NEVER throws. NEVER logs (INV-A3's posture extended to this TU): it RETURNS a result.
[[nodiscard]] OrphanDeleteResult deleteOrphanMeta(std::string_view assetsRootUtf8, std::string_view relativeMetaPath);

}  // namespace engine::editor

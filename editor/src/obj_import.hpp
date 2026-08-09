// editor/src/obj_import.hpp -- SRC-PRIVATE, exactly like gltf_import.hpp and fbx_import.hpp.
// Its ONLY consumer is model_import.cpp. tinyobjloader appears NOWHERE in this header -- only in the
// .cpp. Confining the header is what confines the temptation.
#pragma once
#include <aero/editor/model_import.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::editor {

// The Wavefront backend, for BOTH claimed extensions. `fileName` decides which arm runs -- unlike
// importGltf/importFbx, which each serve one file kind. `.obj`: at Structure depth a PURE TEXT SCAN
// (tinyobjloader is not entered at all, D5); at Full depth, geometry plus whatever .mtl text `external`
// supplied. `.mtl`: materials and image references, IDENTICAL at both depths (D6).
// NEVER READS A FILE, NEVER LOGS, NEVER THROWS, NEVER TOUCHES <filesystem>.
[[nodiscard]] ImportResult importObj(std::string_view fileName, std::string_view assetRelativeDir,
                                     std::span<const std::byte> bytes, const ImportSettings& settings,
                                     ImportDepth depth, std::span<const ExternalBuffer> external);

}  // namespace engine::editor

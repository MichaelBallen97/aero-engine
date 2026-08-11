// editor/src/assimp_import.hpp -- SRC-PRIVATE, exactly like gltf_import.hpp, fbx_import.hpp and
// obj_import.hpp. Its ONLY consumer is model_import.cpp. Assimp appears NOWHERE in this header --
// only in the .cpp. Confining the header is what confines the temptation.
#pragma once
#include <aero/editor/model_import.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::editor {

// The Assimp backend, for all THREE claimed extensions. `fileName` decides which arm runs -- unlike
// importGltf/importFbx, which each serve one file kind, and like importObj, which serves two.
//   .stl : Structure returns an EMPTY model without entering the library at all (STL has no external
//          reference of any kind, so the exact URI set is provably {}).
//   .ply : Structure is a bounded, pure HEADER scan (scanPlyTextureFiles). The library is not entered.
//   .dae : Structure is a real parse with the sample data skipped -- Collada's texture paths are
//          <library_images> entries resolved through <effect>s, and there is no cheap way to get the
//          EXACT set.
// The first two are a STATED DEVIATION from INV-M4 (Structure and Full agree about everything), exactly
// as .obj has been since task 3.2.3; AC-19 asserts the one thing they DO owe -- an identical externalUris.
//
// NEVER READS A FILE, NEVER LOGS, NEVER THROWS, NEVER TOUCHES <filesystem>.
[[nodiscard]] ImportResult importAssimp(std::string_view fileName, std::string_view assetRelativeDir,
                                        std::span<const std::byte> bytes, const ImportSettings& settings,
                                        ImportDepth depth, std::span<const ExternalBuffer> external);

}  // namespace engine::editor

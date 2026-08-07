// editor/src/gltf_import.hpp -- SRC-PRIVATE, exactly like thumbnail_store.hpp (task 3.1.3, D18).
// Its ONLY consumer is model_import.cpp. fastgltf appears NOWHERE in this header -- only in the .cpp.
// Confining the header is what confines the temptation.
#pragma once
#include <aero/editor/model_import.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::editor {

// The glTF/GLB backend. `bytes` is the whole .gltf or .glb file; fastgltf auto-detects which.
// `external` is empty at ImportDepth::Structure and holds every buffer the Structure pass named at
// ImportDepth::Full. NEVER READS A FILE, NEVER LOGS, NEVER THROWS.
[[nodiscard]] ImportResult importGltf(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                                      const ImportSettings& settings, ImportDepth depth,
                                      std::span<const ExternalBuffer> external);

}  // namespace engine::editor

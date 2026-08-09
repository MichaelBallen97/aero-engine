// editor/src/fbx_import.hpp -- SRC-PRIVATE, exactly like gltf_import.hpp and thumbnail_store.hpp.
// Its ONLY consumer is model_import.cpp. ufbx appears NOWHERE in this header -- only in the .cpp.
// Confining the header is what confines the temptation.
#pragma once
#include <aero/editor/model_import.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::editor {

// The FBX backend. `bytes` is the whole .fbx file; ufbx handles binary and ASCII, 6.x and 7.x.
// `external` is ALWAYS EMPTY -- FBX has no external geometry buffers (D5), and the parameter exists
// only so this signature matches importGltf's, so model_import.cpp's dispatch stays uniform.
// NEVER READS A FILE, NEVER LOGS, NEVER THROWS, NEVER TOUCHES <filesystem>.
[[nodiscard]] ImportResult importFbx(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                                     const ImportSettings& settings, ImportDepth depth,
                                     std::span<const ExternalBuffer> external);

}  // namespace engine::editor

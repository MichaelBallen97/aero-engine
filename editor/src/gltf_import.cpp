#include "gltf_import.hpp"
#include <fastgltf/core.hpp>

namespace engine::editor {
ImportResult importGltf(std::string_view, std::span<const std::byte> bytes, const ImportSettings&, ImportDepth,
                        std::span<const ExternalBuffer>) {
    // A STUB, replaced in Steps 5-8. It deliberately calls TWO OUT-OF-LINE symbols so this step proves
    // the LINK and simdjson's dispatch, not merely the include: fastgltf::getErrorName is `constexpr`
    // and would be constant-folded away, proving nothing. Parser's constructor allocates a
    // simdjson::dom::parser (src/fastgltf.cpp), and GltfDataBuffer::FromBytes calls allocateAndCopy
    // (also out-of-line).
    const fastgltf::Parser parser;
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    ImportResult result;
    result.status = ImportStatus::ParseFailed;
    result.message = data.error() == fastgltf::Error::None ? "the glTF backend is not implemented yet"
                                                           : std::string(fastgltf::getErrorName(data.error()));
    return result;
}
}  // namespace engine::editor

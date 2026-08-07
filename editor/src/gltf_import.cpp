#include "gltf_import.hpp"

// The ONE place fastgltf exists in this tree (INV-M1/AC-55). Everything below is confined to this TU:
// no fastgltf type, value or header name escapes into any other file.
//
// <filesystem> arrives TRANSITIVELY through fastgltf/core.hpp and CANNOT BE AVOIDED: Parser::loadGltf
// takes a `std::filesystem::path directory` POSITIONALLY (plan §A-7). That weakens D3's fourth reason
// ("one <filesystem> owner per concern") to something still true and still checkable:
// **THIS FILE PERFORMS NO FILE OPERATION AT ALL** -- no std::filesystem:: call at all, no <fstream>, no
// stream, no open, no stat. §V6's grep is the enforcement. `directory` is passed as `{}`, which
// fastgltf explicitly supports: it validates the path ONLY when Options::LoadExternalBuffers is set
// ("If we never have to load the files ourselves, we're fine with the directory being invalid/blank"),
// and we never set it.
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace engine::editor {
namespace {

// D3 / AC-56: exactly TWO option bits, written ONCE. LoadExternalBuffers, LoadExternalImages,
// LoadGLBBuffers, AllowDouble and DontRequireValidAssetMember must NEVER appear anywhere in this tree.
constexpr fastgltf::Options GLTF_OPTIONS =
    fastgltf::Options::DecomposeNodeMatrices | fastgltf::Options::GenerateMeshIndices;

// A25: the ONE emitter. warningTotal is UNCAPPED; `warnings` stops at MAX_IMPORT_WARNINGS. No call
// site may push_back into `warnings` directly -- sabotage seed S27 targets exactly that.
void addWarning(ImportResult& result, std::string text) {
    ++result.warningTotal;
    if (result.warnings.size() < MAX_IMPORT_WARNINGS) {
        result.warnings.push_back(std::move(text));
    }
}

// A26: MONOTONE escalation, Ok < Truncated < MissingBuffer. The four hard failures are returned
// directly by the phase that detects them and never go through here.
void escalate(ImportResult& result, ImportStatus status, std::string_view why) {
    const auto rank = [](ImportStatus s) -> int {
        switch (s) {
            case ImportStatus::Ok:
                return 0;
            case ImportStatus::Truncated:
                return 1;
            case ImportStatus::MissingBuffer:
                return 2;
            case ImportStatus::Unsupported:
            case ImportStatus::ParseFailed:
            case ImportStatus::Malformed:
            case ImportStatus::MissingExtension:
                return 3;
        }
        return 3;
    };
    if (rank(status) > rank(result.status)) {
        result.status = status;
    }
    if (!why.empty()) {
        if (!result.message.empty()) {
            result.message += "; ";
        }
        result.message += why;
    }
}

// THE SEAM D3 RESTS ON. fastgltf's accessor tools take a template parameter defaulting to
// DefaultBufferDataAdapter -- there is NO concept in 0.9.0 (plan §A-3); the requirement is a callable
//     span<const std::byte> operator()(const Asset&, std::size_t bufferViewIdx) const
//
// CRITICAL, and silently wrong if missed: THE ADAPTER applies the bufferView's own byteOffset and
// byteLength. Every caller then applies ONLY accessor.byteOffset on top -- IterableAccessor's ctor and
// copyFromAccessor both do `adapter(...).subspan(accessor.byteOffset)`. An adapter that returns the
// whole buffer reads from the wrong offset with NO error at all.
//
// ALSO CRITICAL: fastgltf::span is UNCHECKED (subspan is pointer arithmetic; at() is data()[idx]) and
// DefaultBufferDataAdapter range-checks nothing, so a hostile "byteOffset": 4000000000 would read wild
// memory. This adapter bounds-checks and returns an EMPTY span on any violation -- and validateAccessor
// (§D-3.3) has already refused such an accessor, so this is belt AND braces, never the only defence.
class EditorBufferAdapter {
public:
    explicit EditorBufferAdapter(std::span<const ExternalBuffer> external) noexcept : externalBuffers(external) {}

    [[nodiscard]] fastgltf::span<const std::byte> operator()(const fastgltf::Asset& asset,
                                                             std::size_t bufferViewIdx) const {
        if (bufferViewIdx >= asset.bufferViews.size()) {
            return {};
        }
        const fastgltf::BufferView& view = asset.bufferViews[bufferViewIdx];
        if (view.bufferIndex >= asset.buffers.size()) {
            return {};
        }
        const fastgltf::span<const std::byte> whole = bytesOf(asset.buffers[view.bufferIndex]);
        if (view.byteOffset > whole.size() || view.byteLength > whole.size() - view.byteOffset) {
            return {};  // out of range -- refuse rather than read
        }
        return whole.subspan(view.byteOffset, view.byteLength);
    }

    // The variant discrimination D3 exists for. sources::Vector is documented UPSTREAM as export-only
    // ("This type is not used by the fastgltf parser"); it is handled anyway because handling it costs
    // one line and its absence would become a silent zero-length read if that ever changed.
    // sources::CustomBuffer / sources::Fallback / std::monostate are unreachable here (we set no
    // allocation callback and require no meshopt) and each returns empty rather than asserting --
    // DefaultBufferDataAdapter's `assert(false)` would be a Debug abort on a merely-odd document.
    [[nodiscard]] fastgltf::span<const std::byte> bytesOf(const fastgltf::Buffer& buffer) const {
        return std::visit(
            fastgltf::visitor{
                [](const auto&) -> fastgltf::span<const std::byte> { return {}; },
                [](const fastgltf::sources::Array& a) -> fastgltf::span<const std::byte> {
                    // NOTE (deviation from the plan's literal §D-3.2 text, build-time finding): the
                    // installed 0.9.0 fastgltf::span's (Iterator, count) constructor is UNCONDITIONALLY
                    // `explicit` (types.hpp:1766) -- braced copy-list-initialization in a return
                    // statement does not compile against it. Direct-initialize instead.
                    return fastgltf::span<const std::byte>(a.bytes.data(), a.bytes.size_bytes());
                },
                [](const fastgltf::sources::Vector& v) -> fastgltf::span<const std::byte> {
                    return fastgltf::span<const std::byte>(v.bytes.data(), v.bytes.size());
                },
                [](const fastgltf::sources::ByteView& b) -> fastgltf::span<const std::byte> { return b.bytes; },
                [this](const fastgltf::sources::URI& u) -> fastgltf::span<const std::byte> {
                    // The caller supplied it, or nobody did. NEVER a disk read (D3/INV-M3).
                    const std::string_view wanted = u.uri.string();  // ALREADY percent-decoded (A1)
                    for (const ExternalBuffer& e : externalBuffers) {
                        if (e.uri == wanted) {
                            if (u.fileByteOffset > e.bytes.size()) {
                                return {};
                            }
                            const auto* const base = reinterpret_cast<const std::byte*>(e.bytes.data());
                            return fastgltf::span<const std::byte>(base + u.fileByteOffset,
                                                                   e.bytes.size() - u.fileByteOffset);
                        }
                    }
                    missingBuffer = true;  // escalated ONCE by the caller, after the phase
                    return {};
                },
            },
            buffer.data);
    }

    [[nodiscard]] bool sawMissingBuffer() const noexcept { return missingBuffer; }

private:
    std::span<const ExternalBuffer> externalBuffers;
    mutable bool missingBuffer = false;  // mutable: operator() is const by fastgltf's own contract
};

// A6. Called ONLY after loadGltf returned Error::MissingExtensions, i.e. only on an already-failed
// path. Re-parses the SAME bytes with every extension enabled purely to recover the NAMES, then throws
// the Asset away. D20 is intact BY CONSTRUCTION: nothing parsed here outlives this function, so no
// extension is ever honoured -- the Asset is a local that dies at the closing brace.
//
// NOTE THE SPELLING: `Extensions::All` DOES NOT EXIST in 0.9.0 (the `All = ~0u` in types.hpp:302 is
// Category::All). The Extensions enum has None plus bits and a `~` operator, so "everything" is
// `~fastgltf::Extensions::None`. Writing Extensions::All will not compile.
[[nodiscard]] std::string describeRequiredExtensions(std::span<const std::byte> bytes) {
    static constexpr std::string_view FALLBACK =
        "the document requires a glTF extension this importer does not implement";

    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        return std::string(FALLBACK);
    }

    fastgltf::Parser permissive(~fastgltf::Extensions::None);
    auto probed = permissive.loadGltf(data.get(), {}, GLTF_OPTIONS);
    if (probed.error() != fastgltf::Error::None) {
        return std::string(FALLBACK);
    }

    // Cap the join: extensionsRequired is user-supplied and unbounded (D15's posture, applied to a
    // STRING rather than to an allocation).
    std::string names;
    std::size_t shown = 0;
    for (const auto& ext : probed.get().extensionsRequired) {
        if (shown == MAX_REPORTED_REQUIRED_EXTENSIONS) {
            names += std::format(", and {} more", probed.get().extensionsRequired.size() - shown);
            break;
        }
        if (shown != 0) {
            names += ", ";
        }
        names += std::string_view(ext);
        ++shown;
    }
    if (names.empty()) {
        return std::string(FALLBACK);
    }
    return std::format("the document requires {} this importer does not implement: {}",
                       shown == 1 ? "a glTF extension" : "glTF extensions", names);
}

}  // namespace

ImportResult importGltf(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                        const ImportSettings& /*settings*/, ImportDepth /*depth*/,
                        std::span<const ExternalBuffer> external) {
    ImportResult result;
    // Phase 1 -- LOAD.
    // FromBytes COPIES (verified in §G-16 item 1: the ctor calls allocateAndCopy into a member
    // unique_ptr<std::byte[]>), so there is NO lifetime contract on `bytes`. Stated here rather than
    // discovered under ASan, and stated because a future switch to FromSpan WOULD borrow.
    auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
    if (data.error() != fastgltf::Error::None) {
        result.status = ImportStatus::ParseFailed;
        result.message =
            std::format("{} ({})", fastgltf::getErrorMessage(data.error()), fastgltf::getErrorName(data.error()));
        return result;
    }
    fastgltf::Parser parser;  // A19: a FUNCTION-LOCAL, one per call. It owns a simdjson::dom::parser, is
                              // move-only, and is not thread-safe; a `static` one would be shared
                              // mutable state in a function this task promises is pure in behaviour.
    auto parsed = parser.loadGltf(data.get(), {}, GLTF_OPTIONS);  // `{}` -- see the preamble (A7)
    if (parsed.error() != fastgltf::Error::None) {
        // A6: fastgltf FAILS THE WHOLE PARSE on an extensionsRequired entry -- before
        // asset.extensionsRequired is populated -- and neither error names the extension. But the two
        // errors are NOT equivalent, and only ONE of them is recoverable:
        //
        //   MissingExtensions        -- fastgltf KNOWS it (one of the 26 in extensionStrings); it was
        //                               simply not enabled on THIS Parser. Re-parsing with everything
        //                               enabled populates asset.extensionsRequired, so we CAN name it.
        //   UnknownRequiredExtension -- fastgltf does not know it at all (a vendor extension, or one
        //                               newer than 0.9.0). NO Parser setting changes this outcome.
        if (parsed.error() == fastgltf::Error::MissingExtensions) {
            result.status = ImportStatus::MissingExtension;
            result.message = describeRequiredExtensions(bytes);  // the throwaway re-parse, above
            return result;
        }
        if (parsed.error() == fastgltf::Error::UnknownRequiredExtension) {
            result.status = ImportStatus::MissingExtension;
            result.message =
                "the document requires a glTF extension this importer does not implement, "
                "and it is not one fastgltf recognises";
            return result;
        }
        result.status = ImportStatus::ParseFailed;
        result.message =
            std::format("{} ({})", fastgltf::getErrorMessage(parsed.error()), fastgltf::getErrorName(parsed.error()));
        return result;  // AC-40/AC-41/E4/E6: an EMPTY model, always
    }
    const fastgltf::Asset& asset = parsed.get();
    const EditorBufferAdapter adapter(external);

    // Phase 2 -- EXTENSIONS. One capped warning per asset.extensionsUsed entry (AC-41's success half).
    // extensionsRequired never reaches here on a SUCCESSFUL parse (A6) -- a document that has one this
    // build cannot satisfy has already returned from phase 1.
    for (const auto& ext : asset.extensionsUsed) {
        addWarning(result,
                   std::format("uses extension '{}', which this importer does not implement", std::string_view(ext)));
    }

    // Phase 3 -- URIS. Runs at BOTH depths (INV-M4); it is the whole point of Structure. `assetRelativeDir`
    // is used here to resolve every relative image/buffer URI against the model's own directory (D14).
    bool externalUriCapHit = false;
    const auto recordExternalUri = [&](const std::string& path) {
        if (std::find(result.externalUris.begin(), result.externalUris.end(), path) != result.externalUris.end()) {
            return;  // already present -- E8's dedup
        }
        if (result.externalUris.size() >= MAX_EXTERNAL_URIS) {
            if (!externalUriCapHit) {
                escalate(result, ImportStatus::Truncated, "external URI cap (MAX_EXTERNAL_URIS) reached");
                externalUriCapHit = true;
            }
            return;  // stop collecting
        }
        result.externalUris.push_back(path);
    };
    bool embeddedCapHit = false;
    // A8: the true pre-allocation bound is MAX_MODEL_FILE_BYTES (readFileBytes enforces it WITHOUT
    // opening the file); Parser::decodeDataUri already allocated a data: URI's payload during parse,
    // before any of our code ran, so this is necessarily an AFTER-THE-FACT check.
    const auto checkEmbeddedCap = [&](std::size_t byteSize) -> bool {
        if (byteSize <= MAX_EMBEDDED_BYTES) {
            return true;
        }
        if (!embeddedCapHit) {
            escalate(result, ImportStatus::Truncated, "an embedded buffer or image exceeds MAX_EMBEDDED_BYTES");
            embeddedCapHit = true;
        }
        return false;
    };

    for (std::size_t i = 0; i < asset.buffers.size(); ++i) {
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::sources::URI& u) {
                           const UriClassification classified = classifyUri(u.uri.string(), assetRelativeDir);
                           if (classified.kind == UriClass::RelativePath) {
                               recordExternalUri(classified.relativePath);
                           } else if (classified.kind != UriClass::DataUri) {
                               addWarning(result,
                                          std::format("buffer {} ('{}'): {}", i, u.uri.string(), classified.reason));
                           }
                           // DataUri is UNREACHABLE here in practice: fastgltf decodes a data: URI during
                           // parse (§G-16 item 2), so a buffer's sources::URI never actually carries one.
                       },
                       [&](const fastgltf::sources::Array& a) { checkEmbeddedCap(a.bytes.size_bytes()); },
                       [&](const fastgltf::sources::ByteView& b) { checkEmbeddedCap(b.bytes.size()); },
                       [&](const auto&) {},
                   },
                   asset.buffers[i].data);
    }

    for (std::size_t i = 0; i < asset.images.size(); ++i) {
        ImportedImage out;
        std::visit(fastgltf::visitor{
                       [&](const fastgltf::sources::URI& u) {
                           out.uri = std::string(u.uri.string());  // ALREADY percent-decoded (A1)
                           out.mimeType = std::string(fastgltf::getMimeTypeString(u.mimeType));
                           const UriClassification classified = classifyUri(u.uri.string(), assetRelativeDir);
                           if (classified.kind == UriClass::RelativePath) {
                               out.relativePath = classified.relativePath;
                               recordExternalUri(classified.relativePath);
                           } else if (classified.kind == UriClass::DataUri) {
                               out.embedded = true;  // UNREACHABLE in practice (see above); handled anyway
                           } else {
                               out.refusal = classified.reason;
                               addWarning(result,
                                          std::format("image {} ('{}'): {}", i, u.uri.string(), classified.reason));
                           }
                       },
                       [&](const fastgltf::sources::Array& a) {
                           out.mimeType = std::string(fastgltf::getMimeTypeString(a.mimeType));
                           if (checkEmbeddedCap(a.bytes.size_bytes())) {
                               out.embedded = true;
                           } else {
                               out.refusal = "embedded image data exceeds the embedded-data cap (MAX_EMBEDDED_BYTES)";
                           }
                       },
                       [&](const fastgltf::sources::ByteView& b) {
                           out.mimeType = std::string(fastgltf::getMimeTypeString(b.mimeType));
                           if (checkEmbeddedCap(b.bytes.size())) {
                               out.embedded = true;
                           } else {
                               out.refusal = "embedded image data exceeds the embedded-data cap (MAX_EMBEDDED_BYTES)";
                           }
                       },
                       [&](const fastgltf::sources::BufferView& bv) {
                           // A GLB image referencing a bufferView. Its DECLARED byteLength is a claim by the
                           // document, not a fresh allocation (the underlying buffer was already loaded as a
                           // whole when the container was parsed) -- checking it against MAX_EMBEDDED_BYTES here
                           // is therefore the same "before the allocation" shape D15 uses for accessor.count in
                           // phase 6 (MI78), and is what MI46 exercises cheaply without a real oversized payload.
                           out.mimeType = std::string(fastgltf::getMimeTypeString(bv.mimeType));
                           const std::size_t declaredSize = bv.bufferViewIndex < asset.bufferViews.size()
                                                                ? asset.bufferViews[bv.bufferViewIndex].byteLength
                                                                : 0;
                           if (checkEmbeddedCap(declaredSize)) {
                               out.embedded = true;
                           } else {
                               out.refusal = "embedded image data exceeds the embedded-data cap (MAX_EMBEDDED_BYTES)";
                           }
                       },
                       [&](const auto&) {},
                   },
                   asset.images[i].data);
        result.model.images.push_back(std::move(out));
    }
    result.model.summary.imageCount = asset.images.size();

    return result;
}

}  // namespace engine::editor

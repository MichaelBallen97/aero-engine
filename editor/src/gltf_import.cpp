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
    explicit EditorBufferAdapter(std::span<const ExternalBuffer> external, std::string_view assetDir) noexcept
        : externalBuffers(external), assetRelativeDir(assetDir) {}

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
                    //
                    // BLOCKING-1 fix: u.uri.string() is the document's RAW URI ("chair.bin"), but
                    // ExternalBuffer::uri holds classifyUri's RESOLVED, project-relative path
                    // ("models/chair.bin") -- exactly what phase 3 already recorded into
                    // result.externalUris via the SAME function. Comparing the raw form against the
                    // resolved form only ever agreed when assetRelativeDir == "" (a model sitting at the
                    // assets root) -- every ordinary "glTF Separate" export dropped into a subdirectory
                    // silently became MissingBuffer. classifyUri is pure and deterministic, so re-running
                    // it here on the identical (uri, assetRelativeDir) pair reproduces phase 3's own
                    // resolution exactly, with no dependency on result.externalUris' contents.
                    const UriClassification classified = classifyUri(u.uri.string(), assetRelativeDir);
                    if (classified.kind != UriClass::RelativePath) {
                        missingBuffer = true;  // refused or embedded -- never a dependency the caller supplies
                        return {};
                    }
                    const std::string_view wanted = classified.relativePath;
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
    std::string_view assetRelativeDir;   // the model's own directory -- classifyUri's second argument
    mutable bool missingBuffer = false;  // mutable: operator() is const by fastgltf's own contract
};

// Called before EVERY fastgltf tool invocation. fastgltf ASSERTS on a type mismatch (a Debug abort in
// our lanes), DEREFERENCES a possibly-disengaged Optional for a sparse accessor with no bufferView
// (types.hpp's own comment: "Could have no value for sparse morph targets"), and never bounds-checks
// the accessor -> bufferView -> buffer chain. THIS FUNCTION is what makes a hostile or merely broken
// document cost a WARNING instead of a crash or an out-of-bounds read.
//
// Returns true iff every fastgltf tool may safely be called on `accessor` with an ElementType whose
// traits type is `expected`.
[[nodiscard]] bool validateAccessor(const fastgltf::Asset& asset, std::size_t accessorIndex,
                                    fastgltf::AccessorType expected, const EditorBufferAdapter& adapter) {
    if (accessorIndex >= asset.accessors.size()) {
        return false;
    }
    const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
    if (accessor.type != expected || accessor.componentType == fastgltf::ComponentType::Invalid) {
        return false;  // would trip fastgltf's own assert
    }
    if (accessor.count == 0) {
        return false;
    }
    // A4: an accessor with no bufferView is legal glTF (zero-filled, possibly sparse-overridden), and
    // IterableAccessor's ctor dereferences *accessor.bufferViewIndex UNCONDITIONALLY. Refuse it
    // outright: an all-zero POSITION is useless, and a sparse-only accessor is the morph-target shape
    // D12 does not import. ONE RULE, NO SUB-CASES.
    if (!accessor.bufferViewIndex.has_value()) {
        return false;
    }
    const std::size_t viewIndex = *accessor.bufferViewIndex;
    if (viewIndex >= asset.bufferViews.size()) {
        return false;
    }
    const fastgltf::span<const std::byte> viewBytes = adapter(asset, viewIndex);
    if (viewBytes.empty()) {
        return false;  // the adapter already refused it (out of range, or an unsupplied external buffer)
    }
    const fastgltf::BufferView& view = asset.bufferViews[viewIndex];
    const std::size_t elementSize = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
    const std::size_t stride = view.byteStride.has_value() ? *view.byteStride : elementSize;
    if (stride == 0 || elementSize == 0) {
        return false;
    }
    if (accessor.byteOffset > viewBytes.size()) {
        return false;
    }
    // The exact span the tools will touch: byteOffset + stride*(count-1) + elementSize. Written to
    // avoid ANY overflow: compare against the remaining length rather than summing.
    const std::size_t available = viewBytes.size() - accessor.byteOffset;
    const std::size_t maxElements = available >= elementSize ? (available - elementSize) / stride + 1U : 0U;
    if (accessor.count > maxElements) {
        return false;
    }
    // Sparse: the same checks for both of its views, and ONLY when count > 0 (copyFromAccessor falls
    // through to the dense path when sparse->count == 0).
    if (accessor.sparse.has_value() && accessor.sparse->count > 0) {
        const fastgltf::SparseAccessor& s = *accessor.sparse;
        if (s.indicesBufferView >= asset.bufferViews.size() || s.valuesBufferView >= asset.bufferViews.size()) {
            return false;
        }
        if (adapter(asset, s.indicesBufferView).empty() || adapter(asset, s.valuesBufferView).empty()) {
            return false;
        }
    }
    return true;
}

// F7b -- THE MOST IMPORTANT COMMENT IN THIS FILE.
//
// The engine's math conventions were CHOSEN to match glTF 2.0. aero/core/math.hpp's own header comment
// says so and names importers as one of the four consumers that inherit them (ADR-005, task 0.2.2):
// right-handed, Y-up, -Z forward; column-major, Model = T * R * S; radians; Quat is {x,y,z,w}.
//
// VERIFIED AT THE BIT LEVEL against fastgltf 0.9.0's own math.hpp: fastgltf::math::vec stores
// std::array<T,N> with x()==[0]; fastgltf::math::quat stores {x,y,z,w} defaulting to {0,0,0,1};
// fastgltf::math::mat stores std::array<vec<T,N>,M> where "every vec<> here is a COLUMN".
//
// THEREFORE: no handedness flip, no axis swap, no Y/Z exchange, no winding reversal, no quaternion
// component reordering, no matrix transpose, no degree conversion. THE IMPORTER CONVERTS NOTHING.
//
// This is the easiest thing in the whole task to get catastrophically wrong, because "importers
// convert coordinate systems" is true of nearly every other engine and is exactly the reflex a
// conscientious implementer brings. A helpful-looking convertFromGltfSpace() here would mirror every
// model, invert every winding order, and be extremely hard to diagnose from a vertex count.
// THERE IS NO SUCH FUNCTION, and MI40b (AC-30b) plus sabotage seeds S29/S30 exist to prove that adding
// one is caught. 3.2.2 (FBX: Z-up, centimetres) IS the task that needs a conversion, and it needs one
// precisely because glTF is the canonical format these types were built around.
[[nodiscard]] constexpr Vec2 toVec2(const fastgltf::math::fvec2& v) noexcept { return {v[0], v[1]}; }
[[nodiscard]] constexpr Vec3 toVec3(const fastgltf::math::fvec3& v) noexcept { return {v[0], v[1], v[2]}; }
[[nodiscard]] constexpr Vec4 toVec4(const fastgltf::math::fvec4& v) noexcept { return {v[0], v[1], v[2], v[3]}; }
[[nodiscard]] constexpr Quat toQuat(const fastgltf::math::fquat& q) noexcept { return {q[0], q[1], q[2], q[3]}; }
[[nodiscard]] constexpr Mat4 toMat4(const fastgltf::math::fmat4x4& m) noexcept {
    return Mat4{std::array<Vec4, 4>{Vec4{m[0][0], m[0][1], m[0][2], m[0][3]}, Vec4{m[1][0], m[1][1], m[1][2], m[1][3]},
                                    Vec4{m[2][0], m[2][1], m[2][2], m[2][3]},
                                    Vec4{m[3][0], m[3][1], m[3][2], m[3][3]}}};
}

// A13: both sides always qualified. NEVER `using namespace fastgltf;`, and never a `using fastgltf::X;`
// for AlphaMode, AnimationPath or AnimationInterpolation -- all three collide with ours by name.
[[nodiscard]] constexpr AlphaMode toAlphaMode(fastgltf::AlphaMode m) noexcept {
    switch (m) {
        case fastgltf::AlphaMode::Opaque:
            return AlphaMode::Opaque;
        case fastgltf::AlphaMode::Mask:
            return AlphaMode::Mask;
        case fastgltf::AlphaMode::Blend:
            return AlphaMode::Blend;
    }
    return AlphaMode::Opaque;
}
[[nodiscard]] constexpr TextureWrap toWrap(fastgltf::Wrap w) noexcept {
    switch (w) {
        case fastgltf::Wrap::Repeat:
            return TextureWrap::Repeat;
        case fastgltf::Wrap::ClampToEdge:
            return TextureWrap::ClampToEdge;
        case fastgltf::Wrap::MirroredRepeat:
            return TextureWrap::MirroredRepeat;
    }
    return TextureWrap::Repeat;
}
// glTF's six Filter values fold into our (TextureFilter, MipFilter) pair. magFilter has no mip
// component and only ever carries Nearest or Linear.
[[nodiscard]] constexpr TextureFilter toMinMagFilter(fastgltf::Filter f) noexcept {
    switch (f) {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::NearestMipMapLinear:
            return TextureFilter::Nearest;
        case fastgltf::Filter::Linear:
        case fastgltf::Filter::LinearMipMapNearest:
        case fastgltf::Filter::LinearMipMapLinear:
            return TextureFilter::Linear;
    }
    return TextureFilter::Linear;
}
[[nodiscard]] constexpr MipFilter toMipFilter(fastgltf::Filter f) noexcept {
    switch (f) {
        case fastgltf::Filter::Nearest:
        case fastgltf::Filter::Linear:
            return MipFilter::None;
        case fastgltf::Filter::NearestMipMapNearest:
        case fastgltf::Filter::LinearMipMapNearest:
            return MipFilter::Nearest;
        case fastgltf::Filter::NearestMipMapLinear:
        case fastgltf::Filter::LinearMipMapLinear:
            return MipFilter::Linear;
    }
    return MipFilter::Linear;
}
// AC-36 / D12: glTF's `weights` path has NO counterpart here, deliberately. nullopt == skip + warn.
[[nodiscard]] constexpr std::optional<AnimationPath> toAnimationPath(fastgltf::AnimationPath p) noexcept {
    switch (p) {
        case fastgltf::AnimationPath::Translation:
            return AnimationPath::Translation;
        case fastgltf::AnimationPath::Rotation:
            return AnimationPath::Rotation;
        case fastgltf::AnimationPath::Scale:
            return AnimationPath::Scale;
        case fastgltf::AnimationPath::Weights:
            return std::nullopt;
    }
    return std::nullopt;
}
[[nodiscard]] constexpr AnimationInterpolation toInterpolation(fastgltf::AnimationInterpolation i) noexcept {
    switch (i) {
        case fastgltf::AnimationInterpolation::Linear:
            return AnimationInterpolation::Linear;
        case fastgltf::AnimationInterpolation::Step:
            return AnimationInterpolation::Step;
        case fastgltf::AnimationInterpolation::CubicSpline:
            return AnimationInterpolation::CubicSpline;
    }
    return AnimationInterpolation::Linear;
}

[[nodiscard]] constexpr std::string_view primitiveModeName(fastgltf::PrimitiveType t) noexcept {
    switch (t) {
        case fastgltf::PrimitiveType::Points:
            return "POINTS";
        case fastgltf::PrimitiveType::Lines:
            return "LINES";
        case fastgltf::PrimitiveType::LineLoop:
            return "LINE_LOOP";
        case fastgltf::PrimitiveType::LineStrip:
            return "LINE_STRIP";
        case fastgltf::PrimitiveType::Triangles:
            return "TRIANGLES";
        case fastgltf::PrimitiveType::TriangleStrip:
            return "TRIANGLE_STRIP";
        case fastgltf::PrimitiveType::TriangleFan:
            return "TRIANGLE_FAN";
    }
    return "UNKNOWN";
}

// Plan A10: a material slot resolves through the TEXTURE table, and BOTH hops can be absent.
// basisuImageIndex / ddsImageIndex / webpImageIndex are NEVER consulted (D20: no KHR_* extensions --
// and with Extensions::None they are always disengaged anyway).
[[nodiscard]] std::optional<ImportedTextureRef> resolveTextureRef(const fastgltf::Asset& asset,
                                                                  const fastgltf::TextureInfo& info) {
    if (info.textureIndex >= asset.textures.size()) {
        return std::nullopt;
    }
    const fastgltf::Texture& texture = asset.textures[info.textureIndex];
    if (!texture.imageIndex.has_value() || *texture.imageIndex >= asset.images.size()) {
        return std::nullopt;  // AC-27: nullopt, NEVER a zero index
    }
    ImportedTextureRef ref;
    ref.imageIndex = static_cast<std::uint32_t>(*texture.imageIndex);
    ref.uvSet = static_cast<std::uint32_t>(info.texCoordIndex);
    if (texture.samplerIndex.has_value() && *texture.samplerIndex < asset.samplers.size()) {
        const fastgltf::Sampler& s = asset.samplers[*texture.samplerIndex];
        ref.wrapU = toWrap(s.wrapS);
        ref.wrapV = toWrap(s.wrapT);
        if (s.magFilter.has_value()) {
            ref.magFilter = toMinMagFilter(*s.magFilter);
        }
        if (s.minFilter.has_value()) {
            ref.minFilter = toMinMagFilter(*s.minFilter);
            ref.mipFilter = toMipFilter(*s.minFilter);
        }
    }
    // AC-28: an ABSENT sampler leaves every default in place (repeat/repeat, linear) -- the glTF
    // specification's own defaults, and the reason ImportedTextureRef's members carry them.
    return ref;
}

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
                        const ImportSettings& settings, ImportDepth depth, std::span<const ExternalBuffer> external) {
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
    const EditorBufferAdapter adapter(external, assetRelativeDir);

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

    // Phase 4 -- NODES. Cap first (D15): the first N survive, in source order.
    std::size_t nodeCount = asset.nodes.size();
    if (nodeCount > MAX_NODES_PER_MODEL) {
        escalate(result, ImportStatus::Truncated, "node cap (MAX_NODES_PER_MODEL) reached");
        nodeCount = MAX_NODES_PER_MODEL;
    }
    result.model.nodes.resize(nodeCount);

    // Pass 1 -- per node: name, localId, mesh/skin indices, and the TRS. ALWAYS TRS (F5): a `matrix`
    // source is decomposed.
    for (std::size_t i = 0; i < nodeCount; ++i) {
        const fastgltf::Node& node = asset.nodes[i];
        ImportedNode& out = result.model.nodes[i];
        out.name = std::string(node.name);
        out.localId = static_cast<std::uint32_t>(i);
        if (node.meshIndex.has_value()) {
            out.meshIndex = static_cast<std::uint32_t>(*node.meshIndex);
        }
        if (node.skinIndex.has_value()) {
            out.skinIndex = static_cast<std::uint32_t>(*node.skinIndex);
        }
        if (const auto* const trs = std::get_if<fastgltf::TRS>(&node.transform); trs != nullptr) {
            out.translation = toVec3(trs->translation);
            out.rotation = toQuat(trs->rotation);  // {x,y,z,w} -> {x,y,z,w}. NO REORDER (F7b).
            out.scale = toVec3(trs->scale);
        } else if (const auto* const m = std::get_if<fastgltf::math::fmat4x4>(&node.transform); m != nullptr) {
            // Defensive: with DecomposeNodeMatrices set this arm is unreachable, because fastgltf
            // converts during parse. Kept so a future option change cannot silently produce identity
            // transforms.
            fastgltf::math::fvec3 t;
            fastgltf::math::fquat r;
            fastgltf::math::fvec3 s;
            fastgltf::math::decomposeTransformMatrix(*m, s, r, t);  // (matrix, scale, rotation, translation)
            out.translation = toVec3(t);
            out.rotation = toQuat(r);
            out.scale = toVec3(s);
        }
    }
    result.model.summary.nodeCount = result.model.nodes.size();

    // The `matrix`-form warning (AC-19). With Options::DecomposeNodeMatrices set, fastgltf converts a
    // `matrix` node to TRS during parse and DESTROYS the information that the source used a matrix --
    // the fmat4x4 arm above is only reachable when the option is NOT set. F5's warning therefore cannot
    // be emitted from the parsed asset at all. Decided (plan §D-3.5): one aggregate, source-text
    // HEURISTIC warning per document, from a single find over the raw bytes -- for a .glb the JSON
    // chunk is a prefix of `bytes`, so the same find works without parsing the container. A false
    // positive costs one warning; a false negative costs none; it is not load-bearing for correctness.
    if (std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()).find(R"("matrix")") !=
        std::string_view::npos) {
        addWarning(result, "the document uses a node 'matrix'; decomposed to translation/rotation/scale");
    }

    // Pass 2 -- the hierarchy (A24: ONE pass, no visited set, no recursion). A node can never gain a
    // second parent, so `children` forms a forest by construction -- this is INHERENTLY ACYCLIC, and a
    // `children` array that names its own ancestor produces one "already has a parent" warning and no
    // edge, never an infinite loop. misc-no-recursion is satisfied trivially.
    std::vector<std::uint32_t> parentOf(nodeCount, INVALID_SUBASSET);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        for (const std::size_t c : asset.nodes[i].children) {
            if (c >= nodeCount) {
                addWarning(result, std::format("node {}: child index {} is out of range", i, c));
                continue;
            }
            if (parentOf[c] != INVALID_SUBASSET) {
                addWarning(result, std::format("node {}: already has a parent; the first in source order wins", c));
                continue;
            }
            parentOf[c] = static_cast<std::uint32_t>(i);
            result.model.nodes[i].children.push_back(static_cast<std::uint32_t>(c));
        }
    }
    for (std::size_t i = 0; i < nodeCount; ++i) {
        result.model.nodes[i].parent = parentOf[i];
        if (parentOf[i] == INVALID_SUBASSET) {
            result.model.roots.push_back(static_cast<std::uint32_t>(i));  // AC-21: SOURCE ORDER
        }
    }

    // Scale (A22): settings.scale multiplies ONLY roots' translations. Scaling every node's translation
    // would compound the scale down the hierarchy -- the single easiest thing in this task to get
    // wrong.
    for (const std::uint32_t rootIndex : result.model.roots) {
        result.model.nodes[rootIndex].translation *= settings.scale;
    }

    // Phase 5 -- MATERIALS. Skipped entirely when !settings.importMaterials -- phase 3 already ran, so
    // image DEPENDENCIES survive regardless of this setting (AC-29's critical half).
    if (settings.importMaterials) {
        for (std::size_t i = 0; i < asset.materials.size(); ++i) {
            const fastgltf::Material& material = asset.materials[i];
            ImportedMaterial out;
            out.name = std::string(material.name);
            out.localId = static_cast<std::uint32_t>(i);
            out.baseColorFactor = toVec4(material.pbrData.baseColorFactor);
            out.metallicFactor = material.pbrData.metallicFactor;
            out.roughnessFactor = material.pbrData.roughnessFactor;
            out.emissiveFactor = toVec3(material.emissiveFactor);
            out.alphaMode = toAlphaMode(material.alphaMode);
            out.alphaCutoff = material.alphaCutoff;
            out.doubleSided = material.doubleSided;
            // emissiveStrength, ior, dispersion, unlit and every KHR_* sub-struct are IGNORED (D20).
            if (material.normalTexture.has_value()) {
                out.normalScale = material.normalTexture->scale;
                out.normal = resolveTextureRef(asset, *material.normalTexture);
            }
            if (material.occlusionTexture.has_value()) {
                out.occlusionStrength = material.occlusionTexture->strength;
                out.occlusion = resolveTextureRef(asset, *material.occlusionTexture);
            }
            if (material.pbrData.baseColorTexture.has_value()) {
                out.baseColor = resolveTextureRef(asset, *material.pbrData.baseColorTexture);
            }
            if (material.pbrData.metallicRoughnessTexture.has_value()) {
                out.metallicRoughness = resolveTextureRef(asset, *material.pbrData.metallicRoughnessTexture);
            }
            if (material.emissiveTexture.has_value()) {
                out.emissive = resolveTextureRef(asset, *material.emissiveTexture);
            }
            result.model.materials.push_back(std::move(out));
        }
    }
    result.model.summary.materialCount = result.model.materials.size();

    // Phase 6 -- MESHES. Three running caps, checked BEFORE the allocation each bounds (D15):
    // primitives, vertices, indices. Each is a MODEL-wide total, not per-mesh.
    std::size_t primitiveTotal = 0;
    std::size_t vertexTotal = 0;
    std::size_t indexTotal = 0;
    bool primitiveCapHit = false;
    bool vertexCapHit = false;
    bool indexCapHit = false;

    for (std::size_t meshIdx = 0; meshIdx < asset.meshes.size(); ++meshIdx) {
        const fastgltf::Mesh& mesh = asset.meshes[meshIdx];
        ImportedMesh outMesh;
        outMesh.name = std::string(mesh.name);
        outMesh.localId = static_cast<std::uint32_t>(meshIdx);

        for (std::size_t primIdx = 0; primIdx < mesh.primitives.size(); ++primIdx) {
            const fastgltf::Primitive& prim = mesh.primitives[primIdx];

            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                addWarning(result, std::format("mesh '{}' primitive {}: {} is not imported", outMesh.name, primIdx,
                                               primitiveModeName(prim.type)));
                continue;  // D11: the MESH survives; this primitive does not
            }
            if (primitiveTotal >= MAX_PRIMITIVES_PER_MODEL) {
                if (!primitiveCapHit) {
                    escalate(result, ImportStatus::Truncated, "primitive cap (MAX_PRIMITIVES_PER_MODEL) reached");
                    primitiveCapHit = true;
                }
                continue;
            }
            ++primitiveTotal;

            ImportedPrimitive outPrim;
            outPrim.materialIndex = (prim.materialIndex.has_value() && settings.importMaterials)
                                        ? static_cast<std::uint32_t>(*prim.materialIndex)
                                        : INVALID_SUBASSET;

            if (depth == ImportDepth::Structure) {
                // No accessor is touched; attribute NAMES are in the document, not in the buffers, so
                // `attributes` is still recorded here -- INV-M4's shared half. summary.vertexCount /
                // triangleCount and this primitive's own `bounds` stay at their defaults (0 / a point).
                for (const fastgltf::Attribute& attr : prim.attributes) {
                    const std::string_view name = attr.name;
                    if (name == "POSITION") {
                        outPrim.attributes |= VertexAttribute::Position;
                    } else if (name == "NORMAL") {
                        outPrim.attributes |= VertexAttribute::Normal;
                    } else if (name == "TANGENT") {
                        outPrim.attributes |= VertexAttribute::Tangent;
                    } else if (name == "TEXCOORD_0") {
                        outPrim.attributes |= VertexAttribute::TexCoord0;
                    } else if (name == "TEXCOORD_1") {
                        outPrim.attributes |= VertexAttribute::TexCoord1;
                    } else if (name == "COLOR_0") {
                        outPrim.attributes |= VertexAttribute::Color0;
                    } else if (name == "JOINTS_0") {
                        outPrim.attributes |= VertexAttribute::Joints0;
                    } else if (name == "WEIGHTS_0") {
                        outPrim.attributes |= VertexAttribute::Weights0;
                    }
                }
                outMesh.primitives.push_back(std::move(outPrim));
                continue;
            }

            // ImportDepth::Full below. POSITION IS MANDATORY.
            const auto posIt = prim.findAttribute("POSITION");  // an ITERATOR (A2), not a pointer
            if (posIt == prim.attributes.cend() || posIt->accessorIndex >= asset.accessors.size()) {
                addWarning(result, std::format("mesh '{}' primitive {}: POSITION is missing or invalid", outMesh.name,
                                               primIdx));
                continue;
            }
            // CAP BEFORE THE ALLOCATION -- and before the FULL bounds check (D15/A5): `count` is READ
            // FROM THE DOCUMENT'S OWN ACCESSOR METADATA, which costs nothing regardless of how large it
            // claims to be. validateAccessor's own bounds check requires REAL backing bytes proportional
            // to `count` to ever return true, so checking the cap FIRST is what makes "a document
            // claiming 4 billion vertices costs NOTHING" achievable -- reversing this order would make
            // the cap unreachable without first supplying (or being refused for lacking) 4 billion
            // vertices' worth of real data.
            const std::size_t claimedVertexCount = asset.accessors[posIt->accessorIndex].count;
            if (vertexTotal + claimedVertexCount > MAX_VERTICES_PER_MODEL) {
                if (!vertexCapHit) {
                    escalate(result, ImportStatus::Truncated, "vertex cap (MAX_VERTICES_PER_MODEL) reached");
                    vertexCapHit = true;
                }
                continue;
            }
            if (!validateAccessor(asset, posIt->accessorIndex, fastgltf::AccessorType::Vec3, adapter)) {
                addWarning(result, std::format("mesh '{}' primitive {}: POSITION is missing or invalid", outMesh.name,
                                               primIdx));
                continue;
            }
            const fastgltf::Accessor& posAccessor = asset.accessors[posIt->accessorIndex];
            const std::size_t vertexCount = posAccessor.count;
            outPrim.positions.reserve(vertexCount);
            for (const fastgltf::math::fvec3 v :
                 fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, posAccessor, adapter)) {
                outPrim.positions.push_back(toVec3(v) * settings.scale);
            }
            outPrim.attributes |= VertexAttribute::Position;
            vertexTotal += vertexCount;

            // NORMAL -- Vec3, NEVER scaled.
            if (const auto it = prim.findAttribute("NORMAL"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec3, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.normals.reserve(vertexCount);
                    for (const fastgltf::math::fvec3 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, acc, adapter)) {
                        outPrim.normals.push_back(toVec3(v));
                    }
                    outPrim.attributes |= VertexAttribute::Normal;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: NORMAL is invalid or its count "
                                                   "does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // TANGENT -- Vec4; .w is glTF's bitangent SIGN, never a magnitude. NEVER scaled.
            if (const auto it = prim.findAttribute("TANGENT"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec4, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.tangents.reserve(vertexCount);
                    for (const fastgltf::math::fvec4 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, acc, adapter)) {
                        outPrim.tangents.push_back(toVec4(v));
                    }
                    outPrim.attributes |= VertexAttribute::Tangent;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: TANGENT is invalid or its count "
                                                   "does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // TEXCOORD_0 / TEXCOORD_1 -- Vec2.
            if (const auto it = prim.findAttribute("TEXCOORD_0"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec2, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.uv0.reserve(vertexCount);
                    for (const fastgltf::math::fvec2 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec2>(asset, acc, adapter)) {
                        outPrim.uv0.push_back(toVec2(v));
                    }
                    outPrim.attributes |= VertexAttribute::TexCoord0;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: TEXCOORD_0 is invalid or its "
                                                   "count does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }
            if (const auto it = prim.findAttribute("TEXCOORD_1"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec2, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.uv1.reserve(vertexCount);
                    for (const fastgltf::math::fvec2 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec2>(asset, acc, adapter)) {
                        outPrim.uv1.push_back(toVec2(v));
                    }
                    outPrim.attributes |= VertexAttribute::TexCoord1;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: TEXCOORD_1 is invalid or its "
                                                   "count does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // COLOR_0 -- linear RGBA; a VEC3 source is read as fvec3 and widened with a = 1. The
            // accessor's OWN type decides which of the two tools to call.
            if (const auto it = prim.findAttribute("COLOR_0"); it != prim.attributes.cend()) {
                const std::size_t colorIdx = it->accessorIndex;
                const bool declaresVec3 =
                    colorIdx < asset.accessors.size() && asset.accessors[colorIdx].type == fastgltf::AccessorType::Vec3;
                const bool declaresVec4 =
                    colorIdx < asset.accessors.size() && asset.accessors[colorIdx].type == fastgltf::AccessorType::Vec4;
                if (declaresVec3 && validateAccessor(asset, colorIdx, fastgltf::AccessorType::Vec3, adapter) &&
                    asset.accessors[colorIdx].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[colorIdx];
                    outPrim.colors.reserve(vertexCount);
                    for (const fastgltf::math::fvec3 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, acc, adapter)) {
                        const Vec3 c = toVec3(v);
                        outPrim.colors.push_back(Vec4{c.x, c.y, c.z, 1.0F});
                    }
                    outPrim.attributes |= VertexAttribute::Color0;
                } else if (declaresVec4 && validateAccessor(asset, colorIdx, fastgltf::AccessorType::Vec4, adapter) &&
                           asset.accessors[colorIdx].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[colorIdx];
                    outPrim.colors.reserve(vertexCount);
                    for (const fastgltf::math::fvec4 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, acc, adapter)) {
                        outPrim.colors.push_back(toVec4(v));
                    }
                    outPrim.attributes |= VertexAttribute::Color0;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: COLOR_0 is invalid or its count "
                                                   "does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // JOINTS_0 -- read as fastgltf::math::u16vec4 (there is NO ElementTraits for std::array) and
            // copied component-wise; UNSIGNED_BYTE and UNSIGNED_SHORT sources both widen to uint16_t via
            // the SAME static_cast path (verified in tools.hpp's convertComponent -- AC-24's sibling).
            if (const auto it = prim.findAttribute("JOINTS_0"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec4, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.joints.reserve(vertexCount);
                    for (const fastgltf::math::u16vec4 v :
                         fastgltf::iterateAccessor<fastgltf::math::u16vec4>(asset, acc, adapter)) {
                        outPrim.joints.push_back(std::array<std::uint16_t, 4>{v[0], v[1], v[2], v[3]});
                    }
                    outPrim.attributes |= VertexAttribute::Joints0;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: JOINTS_0 is invalid or its "
                                                   "count does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // WEIGHTS_0 -- Vec4.
            if (const auto it = prim.findAttribute("WEIGHTS_0"); it != prim.attributes.cend()) {
                if (validateAccessor(asset, it->accessorIndex, fastgltf::AccessorType::Vec4, adapter) &&
                    asset.accessors[it->accessorIndex].count == vertexCount) {
                    const fastgltf::Accessor& acc = asset.accessors[it->accessorIndex];
                    outPrim.weights.reserve(vertexCount);
                    for (const fastgltf::math::fvec4 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, acc, adapter)) {
                        outPrim.weights.push_back(toVec4(v));
                    }
                    outPrim.attributes |= VertexAttribute::Weights0;
                } else {
                    addWarning(result, std::format("mesh '{}' primitive {}: WEIGHTS_0 is invalid or its "
                                                   "count does not match POSITION",
                                                   outMesh.name, primIdx));
                }
            }

            // INDICES: prim.indicesAccessor is ALWAYS engaged (GenerateMeshIndices, F6). Disengaged
            // anyway -> warn, skip the primitive.
            if (!prim.indicesAccessor.has_value() || *prim.indicesAccessor >= asset.accessors.size()) {
                addWarning(result, std::format("mesh '{}' primitive {}: indices are missing or invalid", outMesh.name,
                                               primIdx));
                continue;
            }
            // CAP BEFORE THE ALLOCATION AND BEFORE validateAccessor, for the identical reason as the
            // vertex cap above: the declared count costs nothing to read regardless of its size.
            const std::size_t claimedIndexCount = asset.accessors[*prim.indicesAccessor].count;
            if (indexTotal + claimedIndexCount > MAX_INDICES_PER_MODEL) {
                if (!indexCapHit) {
                    escalate(result, ImportStatus::Truncated, "index cap (MAX_INDICES_PER_MODEL) reached");
                    indexCapHit = true;
                }
                continue;
            }
            if (!validateAccessor(asset, *prim.indicesAccessor, fastgltf::AccessorType::Scalar, adapter)) {
                addWarning(result, std::format("mesh '{}' primitive {}: indices are missing or invalid", outMesh.name,
                                               primIdx));
                continue;
            }
            const fastgltf::Accessor& indexAccessor = asset.accessors[*prim.indicesAccessor];
            outPrim.indices.reserve(indexAccessor.count);
            // iterateAccessor<uint32_t> normalises UNSIGNED_BYTE/SHORT/INT in ONE path (AC-24).
            for (const std::uint32_t idx : fastgltf::iterateAccessor<std::uint32_t>(asset, indexAccessor, adapter)) {
                outPrim.indices.push_back(idx);
            }
            indexTotal += indexAccessor.count;

            bool sawOutOfRangeIndex = false;
            for (const std::uint32_t idx : outPrim.indices) {
                if (idx >= outPrim.positions.size()) {
                    sawOutOfRangeIndex = true;
                    break;
                }
            }
            if (sawOutOfRangeIndex) {
                // A downstream consumer must never receive an out-of-range index -- skip the WHOLE
                // primitive, never a partial/clamped one.
                addWarning(result, std::format("mesh '{}' primitive {}: an index is out of range of "
                                               "POSITION",
                                               outMesh.name, primIdx));
                continue;
            }
            if (outPrim.indices.size() % 3 != 0) {
                addWarning(result, std::format("mesh '{}' primitive {}: index count is not a multiple of "
                                               "3; truncated",
                                               outMesh.name, primIdx));
                outPrim.indices.resize((outPrim.indices.size() / 3) * 3);
            }

            // BOUNDS: fold over the ALREADY-SCALED positions. The document's own accessor.min/max are
            // DELIBERATELY IGNORED (A21).
            Aabb primBounds = Aabb::empty();
            for (const Vec3& p : outPrim.positions) {
                primBounds.expand(p);
            }
            outPrim.bounds = primBounds;
            outMesh.bounds.expand(primBounds);

            result.model.summary.vertexCount += outPrim.positions.size();
            result.model.summary.triangleCount += outPrim.indices.size() / 3;
            ++result.model.summary.primitiveCount;

            outMesh.primitives.push_back(std::move(outPrim));
        }
        result.model.meshes.push_back(std::move(outMesh));
    }
    result.model.summary.meshCount = result.model.meshes.size();
    if (depth == ImportDepth::Full) {
        for (const ImportedMesh& m : result.model.meshes) {
            result.model.summary.bounds.expand(m.bounds);
        }
    }

    // Phase 7 -- SKINS. Skipped entirely when !settings.importSkins (AC-37).
    if (settings.importSkins) {
        bool jointCapHit = false;
        for (std::size_t skinIdx = 0; skinIdx < asset.skins.size(); ++skinIdx) {
            const fastgltf::Skin& skin = asset.skins[skinIdx];
            ImportedSkin outSkin;
            outSkin.name = std::string(skin.name);
            outSkin.localId = static_cast<std::uint32_t>(skinIdx);
            // E24: recorded AS-IS, even when the skeleton root is not among `joints` below -- it is
            // never cross-validated against that list.
            outSkin.skeletonRoot =
                skin.skeleton.has_value() ? static_cast<std::uint32_t>(*skin.skeleton) : INVALID_SUBASSET;

            if (skin.joints.size() > MAX_JOINTS_PER_SKIN) {
                if (!jointCapHit) {
                    escalate(result, ImportStatus::Truncated, "joint cap (MAX_JOINTS_PER_SKIN) reached");
                    jointCapHit = true;
                }
                continue;  // a partial palette is worse than none
            }

            bool jointOutOfRange = false;
            outSkin.joints.reserve(skin.joints.size());
            for (const std::size_t joint : skin.joints) {  // AC-31: SOURCE ORDER
                if (joint >= result.model.nodes.size()) {
                    jointOutOfRange = true;
                    break;
                }
                outSkin.joints.push_back(static_cast<std::uint32_t>(joint));
            }
            if (jointOutOfRange) {
                addWarning(result, std::format("skin '{}': a joint index is out of range", outSkin.name));
                continue;  // a partial palette is worse than none
            }

            if (!skin.inverseBindMatrices.has_value()) {
                // AC-32: joints.size() IDENTITY matrices, NEVER an empty vector. Computed at BOTH
                // depths -- there is no accessor here to defer, exactly like a node's own TRS.
                outSkin.inverseBindMatrices.assign(outSkin.joints.size(), Mat4::identity());
            } else if (depth == ImportDepth::Full) {
                const std::size_t ibmIndex = *skin.inverseBindMatrices;
                if (!validateAccessor(asset, ibmIndex, fastgltf::AccessorType::Mat4, adapter)) {
                    addWarning(result, std::format("skin '{}': inverse bind matrices are invalid", outSkin.name));
                    continue;
                }
                const fastgltf::Accessor& ibmAccessor = asset.accessors[ibmIndex];
                if (ibmAccessor.count != outSkin.joints.size()) {
                    // AC-33: a mismatch is a warning and the WHOLE skin is skipped -- never a silently
                    // truncated palette.
                    addWarning(result, std::format("skin '{}': {} inverse bind matrices for {} joints", outSkin.name,
                                                   ibmAccessor.count, outSkin.joints.size()));
                    continue;
                }
                outSkin.inverseBindMatrices.reserve(ibmAccessor.count);
                for (const fastgltf::math::fmat4x4 m :
                     fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(asset, ibmAccessor, adapter)) {
                    Mat4 mat = toMat4(m);
                    // A22: settings.scale multiplies ONLY the translation column.
                    mat.columns[3].x *= settings.scale;
                    mat.columns[3].y *= settings.scale;
                    mat.columns[3].z *= settings.scale;
                    outSkin.inverseBindMatrices.push_back(mat);
                }
            }
            // else: engaged, Structure depth -- leave EMPTY. INV-M7's size equality is asserted only
            // at Full depth.

            result.model.summary.jointCount += outSkin.joints.size();
            result.model.skins.push_back(std::move(outSkin));
        }
    }
    result.model.summary.skinCount = result.model.skins.size();

    // Phase 8 -- ANIMATIONS. Skipped entirely when !settings.importAnimations (AC-37).
    if (settings.importAnimations) {
        std::size_t keyTotal = 0;
        bool keyCapHit = false;
        for (std::size_t animIdx = 0; animIdx < asset.animations.size(); ++animIdx) {
            const fastgltf::Animation& animation = asset.animations[animIdx];
            ImportedAnimation outAnim;
            outAnim.name = std::string(animation.name);
            outAnim.localId = static_cast<std::uint32_t>(animIdx);

            for (std::size_t channelIdx = 0; channelIdx < animation.channels.size(); ++channelIdx) {
                const fastgltf::AnimationChannel& channel = animation.channels[channelIdx];

                const std::optional<AnimationPath> path = toAnimationPath(channel.path);
                if (!path.has_value()) {
                    // AC-36/D12: a 'weights' (morph) channel has no counterpart here; the clip's other
                    // channels import normally.
                    addWarning(result, std::format("animation '{}' channel {}: a 'weights' (morph) "
                                                   "channel is not imported",
                                                   outAnim.name, channelIdx));
                    continue;
                }
                if (!channel.nodeIndex.has_value() || *channel.nodeIndex >= result.model.nodes.size()) {
                    addWarning(result, std::format("animation '{}' channel {}: the target node is "
                                                   "missing or out of range",
                                                   outAnim.name, channelIdx));
                    continue;  // E23: the clip survives
                }
                if (channel.samplerIndex >= animation.samplers.size()) {
                    addWarning(result, std::format("animation '{}' channel {}: the sampler index is "
                                                   "out of range",
                                                   outAnim.name, channelIdx));
                    continue;
                }
                const fastgltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];

                ImportedAnimationChannel outChannel;
                outChannel.targetNode = static_cast<std::uint32_t>(*channel.nodeIndex);
                outChannel.path = *path;
                outChannel.interpolation = toInterpolation(sampler.interpolation);

                if (depth == ImportDepth::Structure) {
                    // No accessor is touched; `times`/`values` stay EMPTY and `duration` stays 0 -- the
                    // panel must not claim a duration it did not read.
                    outAnim.channels.push_back(std::move(outChannel));
                    continue;
                }

                // ImportDepth::Full below.
                if (sampler.inputAccessor >= asset.accessors.size() ||
                    sampler.outputAccessor >= asset.accessors.size()) {
                    addWarning(result, std::format("animation '{}' channel {}: a sampler accessor is "
                                                   "missing or invalid",
                                                   outAnim.name, channelIdx));
                    continue;
                }
                // CAP BEFORE THE ALLOCATION (D15), the identical shape phase 6 uses: `count` is read
                // from the document's own accessor metadata, which costs nothing regardless of how
                // large it claims to be.
                const std::size_t claimedKeyCount = asset.accessors[sampler.inputAccessor].count;
                if (keyTotal + claimedKeyCount > MAX_ANIMATION_KEYS_PER_MODEL) {
                    if (!keyCapHit) {
                        escalate(result, ImportStatus::Truncated,
                                 "animation key cap (MAX_ANIMATION_KEYS_PER_MODEL) reached");
                        keyCapHit = true;
                    }
                    continue;
                }
                if (!validateAccessor(asset, sampler.inputAccessor, fastgltf::AccessorType::Scalar, adapter)) {
                    addWarning(result, std::format("animation '{}' channel {}: the time accessor is invalid",
                                                   outAnim.name, channelIdx));
                    continue;
                }
                const fastgltf::AccessorType outputType = outChannel.path == AnimationPath::Rotation
                                                              ? fastgltf::AccessorType::Vec4
                                                              : fastgltf::AccessorType::Vec3;
                if (!validateAccessor(asset, sampler.outputAccessor, outputType, adapter)) {
                    addWarning(result, std::format("animation '{}' channel {}: the value accessor is invalid",
                                                   outAnim.name, channelIdx));
                    continue;
                }

                const fastgltf::Accessor& inputAccessor = asset.accessors[sampler.inputAccessor];
                std::vector<float> times;
                times.reserve(inputAccessor.count);
                for (const float v : fastgltf::iterateAccessor<float>(asset, inputAccessor, adapter)) {
                    times.push_back(v);
                }
                bool notIncreasing = false;
                for (std::size_t k = 1; k < times.size(); ++k) {
                    if (times[k] <= times[k - 1]) {
                        notIncreasing = true;
                        break;
                    }
                }
                if (notIncreasing) {
                    // AC-34: skipped, NEVER sorted -- a clip whose keys are out of order is a broken
                    // export, and silently reordering it produces plausible-looking wrong motion.
                    addWarning(result, std::format("animation '{}' channel {}: keyframe times are not "
                                                   "strictly increasing",
                                                   outAnim.name, channelIdx));
                    continue;
                }

                const fastgltf::Accessor& outputAccessor = asset.accessors[sampler.outputAccessor];
                const std::size_t expectedValueCount =
                    times.size() * (outChannel.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
                if (outputAccessor.count != expectedValueCount) {
                    // INV-M6: a mismatch is skipped, never partially read.
                    addWarning(result, std::format("animation '{}' channel {}: the value count does "
                                                   "not match the key count",
                                                   outAnim.name, channelIdx));
                    continue;
                }

                std::vector<Vec4> values;
                values.reserve(outputAccessor.count);
                if (outChannel.path == AnimationPath::Rotation) {
                    for (const fastgltf::math::fvec4 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, outputAccessor, adapter)) {
                        values.push_back(toVec4(v));
                    }
                } else {
                    // Translation/Scale: widened to Vec4 with w = 0 for glTF's three-component paths.
                    for (const fastgltf::math::fvec3 v :
                         fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, outputAccessor, adapter)) {
                        values.push_back(Vec4{v[0], v[1], v[2], 0.0F});
                    }
                }

                keyTotal += inputAccessor.count;
                const float lastTime = times.back();
                outChannel.times = std::move(times);
                outChannel.values = std::move(values);
                outAnim.duration = std::max(outAnim.duration, lastTime);
                outAnim.channels.push_back(std::move(outChannel));
            }

            result.model.animations.push_back(std::move(outAnim));
        }
    }
    result.model.summary.animationCount = result.model.animations.size();
    for (const ImportedAnimation& anim : result.model.animations) {
        result.model.summary.animationDuration = std::max(result.model.summary.animationDuration, anim.duration);
    }

    if (adapter.sawMissingBuffer()) {
        escalate(result, ImportStatus::MissingBuffer, "an external buffer this document names was not supplied");
    }

    return result;
}

}  // namespace engine::editor

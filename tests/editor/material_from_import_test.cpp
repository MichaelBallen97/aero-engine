// tests/editor/material_from_import_test.cpp -- task 3.1.5, Step 10: the ImportedMaterial ->
// MaterialDocument materializer (MF1-MF20). A TU of aero_editor_shell_test, which supplies main()
// from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (the material_edit_test.cpp precedent), tier-0: no GPU, no window, no ImGui context, no
// disk. Every case is provable from an ImportedMaterial literal with no context of any kind.
//
// THE ME24 LESSON, APPLIED BEFORE IT CAN BITE. 3.4.2 shipped a case that compared a mapping function
// against itself (`desc.addressU == materialAddressModeFor(wrap)`), which cannot see a swapped
// clamp/mirror AT ALL. Every mirror case below asserts against a LITERAL enumerator, and MF18 asserts
// the composed sampler against rhi:: literals -- never against the mapping being tested.
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md). Enum CHECKs use the
// DOUBLE-PAREN posture -- CHECK((a == b)) -- and no toString overload is added anywhere.
#include <aero/core/guid.hpp>
#include <aero/editor/material_edit.hpp>
#include <aero/editor/material_from_import.hpp>
#include <aero/editor/model_import.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/types.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using engine::Guid;
using engine::MATERIAL_MAX_UV_SETS;
using engine::MaterialAlphaMode;
using engine::MaterialDocument;
using engine::MaterialFilter;
using engine::MaterialMipFilter;
using engine::MaterialTextureSlot;
using engine::MaterialWrap;
using engine::validateMaterial;
using engine::Vec3;
using engine::Vec4;
using engine::editor::AlphaMode;
using engine::editor::documentSlotAt;
using engine::editor::ImportedImage;
using engine::editor::ImportedMaterial;
using engine::editor::ImportedTextureRef;
using engine::editor::materialAlphaModeFromImported;
using engine::editor::materialDocumentFromImported;
using engine::editor::materialFilterFromImported;
using engine::editor::MaterialFromImportResult;
using engine::editor::materialMipFilterFromImported;
using engine::editor::materialSamplerDescFor;
using engine::editor::materialSlotIsSrgb;
using engine::editor::materialSlotLabel;
using engine::editor::materialWrapFromImported;
using engine::editor::MipFilter;
using engine::editor::TextureFilter;
using engine::editor::TextureWrap;

namespace {

constexpr Guid WOOD_GUID{0xAAAABBBBCCCCDDDDULL, 0x1111222233334444ULL};
constexpr Guid METAL_GUID{0x0000000000000001ULL, 0x0000000000000002ULL};

[[nodiscard]] ImportedImage resolvedImage(std::string uri, Guid guid) {
    ImportedImage image;
    image.uri = std::move(uri);
    image.relativePath = image.uri;
    image.guid = guid;
    return image;
}

[[nodiscard]] bool anyWarningContains(const MaterialFromImportResult& result, std::string_view needle) {
    for (const std::string& warning : result.warnings) {
        if (warning.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("material_from_import: a default material maps to a default document (MF1)") {
    // ONE LINE that pins the whole default equality: both sides carry glTF 2.0's own defaults
    // verbatim, so this is a property rather than a coincidence. It CANNOT carry S26 (a swapped
    // metallic/roughness), because both defaults are 1.0 -- which is exactly why MF2 exists.
    const MaterialFromImportResult result = materialDocumentFromImported(ImportedMaterial{}, {});
    CHECK(result.document == MaterialDocument{});
    CHECK(result.warnings.empty());
}

TEST_CASE("material_from_import: every factor lands in its own field, against LITERALS (MF2, S26)") {
    ImportedMaterial source;
    source.name = "Brass";
    source.baseColorFactor = Vec4{0.1F, 0.2F, 0.3F, 0.4F};
    source.metallicFactor = 0.25F;   // DISTINCT literals: a swapped pair is visible here and nowhere
    source.roughnessFactor = 0.75F;  // else in this battery
    source.emissiveFactor = Vec3{1.5F, 0.0F, 2.5F};
    source.normalScale = 3.5F;
    source.occlusionStrength = 0.125F;
    source.alphaMode = AlphaMode::Mask;
    source.alphaCutoff = 0.875F;
    source.doubleSided = true;

    const MaterialDocument document = materialDocumentFromImported(source, {}).document;
    CHECK(document.name == "Brass");
    CHECK(document.baseColorFactor == Vec4{0.1F, 0.2F, 0.3F, 0.4F});
    CHECK(document.metallicFactor == doctest::Approx(0.25F));
    CHECK(document.roughnessFactor == doctest::Approx(0.75F));
    CHECK(document.emissiveFactor == Vec3{1.5F, 0.0F, 2.5F});
    CHECK(document.normalScale == doctest::Approx(3.5F));
    CHECK(document.occlusionStrength == doctest::Approx(0.125F));
    CHECK((document.alphaMode == MaterialAlphaMode::Mask));
    CHECK(document.alphaCutoff == doctest::Approx(0.875F));
    CHECK(document.doubleSided);
}

TEST_CASE("material_from_import: materialAlphaModeFromImported, every enumerator (MF3)") {
    CHECK((materialAlphaModeFromImported(AlphaMode::Opaque) == MaterialAlphaMode::Opaque));
    CHECK((materialAlphaModeFromImported(AlphaMode::Mask) == MaterialAlphaMode::Mask));
    CHECK((materialAlphaModeFromImported(AlphaMode::Blend) == MaterialAlphaMode::Blend));
}

TEST_CASE("material_from_import: materialWrapFromImported, every enumerator (MF4, S27)") {
    // Against LITERALS. TextureWrap and MaterialWrap do NOT share spellings -- ClampToEdge -> Clamp,
    // MirroredRepeat -> Mirror -- and this is the case a swapped pair reddens.
    CHECK((materialWrapFromImported(TextureWrap::Repeat) == MaterialWrap::Repeat));
    CHECK((materialWrapFromImported(TextureWrap::ClampToEdge) == MaterialWrap::Clamp));
    CHECK((materialWrapFromImported(TextureWrap::MirroredRepeat) == MaterialWrap::Mirror));
}

TEST_CASE("material_from_import: materialFilterFromImported, every enumerator (MF5)") {
    CHECK((materialFilterFromImported(TextureFilter::Nearest) == MaterialFilter::Nearest));
    CHECK((materialFilterFromImported(TextureFilter::Linear) == MaterialFilter::Linear));
}

TEST_CASE("material_from_import: materialMipFilterFromImported, every enumerator (MF6)") {
    CHECK((materialMipFilterFromImported(MipFilter::None) == MaterialMipFilter::None));
    CHECK((materialMipFilterFromImported(MipFilter::Nearest) == MaterialMipFilter::Nearest));
    CHECK((materialMipFilterFromImported(MipFilter::Linear) == MaterialMipFilter::Linear));
}

TEST_CASE("material_from_import: an out-of-range imageIndex omits the slot and warns (MF7)") {
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 4};
    const std::array<ImportedImage, 1> images{resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK_FALSE(documentSlotAt(result.document, 0).has_value());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0] == "baseColor: the source names image 4, which this model does not have");

    SUBCASE("and so does an UNBOUND ref, whose imageIndex is the INVALID_SUBASSET sentinel") {
        ImportedMaterial sentinel;
        sentinel.normal = ImportedTextureRef{};  // imageIndex defaults to INVALID_SUBASSET
        const MaterialFromImportResult sentinelResult = materialDocumentFromImported(sentinel, images);
        CHECK_FALSE(documentSlotAt(sentinelResult.document, 2).has_value());
        REQUIRE(sentinelResult.warnings.size() == 1);
        CHECK(anyWarningContains(sentinelResult, "normal: the source names image "));
    }
}

TEST_CASE("material_from_import: an EMBEDDED image omits the slot and the warning names it (MF8, S25)") {
    ImportedMaterial source;
    source.emissive = ImportedTextureRef{.imageIndex = 0};
    ImportedImage embedded;
    embedded.embedded = true;
    const std::array<ImportedImage, 1> images{embedded};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    // OMITTED, never bound with a nil guid: a present-but-nil slot is a contradiction section 11
    // forbids. Binding one instead of omitting is S25, and it reddens both halves of this case.
    CHECK_FALSE(documentSlotAt(result.document, 4).has_value());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0] ==
          "emissive: the source embeds this texture in the model file; extract it beside the model to use it");
}

TEST_CASE("material_from_import: a REFUSED image omits the slot and carries the refusal text (MF9)") {
    ImportedMaterial source;
    source.occlusion = ImportedTextureRef{.imageIndex = 0};
    ImportedImage refused;
    refused.uri = "http://evil/x.png";
    refused.refusal = "a remote URI is never read";
    const std::array<ImportedImage, 1> images{refused};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK_FALSE(documentSlotAt(result.document, 3).has_value());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0] == "occlusion: a remote URI is never read");
}

TEST_CASE("material_from_import: a NIL-guid image omits the slot and names the uri (MF10)") {
    ImportedMaterial source;
    source.metallicRoughness = ImportedTextureRef{.imageIndex = 0};
    ImportedImage unresolved;
    unresolved.uri = "textures/orm.png";
    unresolved.relativePath = "textures/orm.png";  // resolved, but no asset claims it
    const std::array<ImportedImage, 1> images{unresolved};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK_FALSE(documentSlotAt(result.document, 1).has_value());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0] == "metallicRoughness: 'textures/orm.png' is not an asset in this project");
}

TEST_CASE("material_from_import: a resolvable image binds with the right guid and tokens (MF11)") {
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 1,
                                          .uvSet = 0,
                                          .wrapU = TextureWrap::ClampToEdge,
                                          .wrapV = TextureWrap::MirroredRepeat,
                                          .minFilter = TextureFilter::Nearest,
                                          .magFilter = TextureFilter::Linear,
                                          .mipFilter = MipFilter::None};
    const std::array<ImportedImage, 2> images{resolvedImage("metal.png", METAL_GUID),
                                              resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    const std::optional<MaterialTextureSlot>& slot = documentSlotAt(result.document, 0);
    REQUIRE(slot.has_value());
    CHECK(slot->guid == WOOD_GUID);  // image 1, not image 0
    CHECK(slot->uvSet == 0);
    CHECK((slot->wrapU == MaterialWrap::Clamp));
    CHECK((slot->wrapV == MaterialWrap::Mirror));
    CHECK((slot->minFilter == MaterialFilter::Nearest));
    CHECK((slot->magFilter == MaterialFilter::Linear));
    CHECK((slot->mipFilter == MaterialMipFilter::None));
    CHECK(result.warnings.empty());
}

TEST_CASE("material_from_import: uvSet 0 binds unclamped and warns nothing (MF12)") {
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 0, .uvSet = 0};
    const std::array<ImportedImage, 1> images{resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    REQUIRE(documentSlotAt(result.document, 0).has_value());
    CHECK(documentSlotAt(result.document, 0)->uvSet == 0);
    CHECK(result.warnings.empty());
}

TEST_CASE("material_from_import: an over-limit uvSet is CLAMPED, with its own warning (MF13)") {
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 0, .uvSet = 7};
    const std::array<ImportedImage, 1> images{resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    REQUIRE(documentSlotAt(result.document, 0).has_value());
    CHECK(documentSlotAt(result.document, 0)->uvSet == MATERIAL_MAX_UV_SETS - 1U);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0] == "baseColor: UV set 7 is beyond this format's limit; using 3");
    // and the slot is still BOUND -- a clamp degrades a value, it does not drop a reference
    CHECK(documentSlotAt(result.document, 0)->guid == WOOD_GUID);

    SUBCASE("the boundary value itself clamps") {
        ImportedMaterial atLimit;
        atLimit.baseColor = ImportedTextureRef{.imageIndex = 0, .uvSet = MATERIAL_MAX_UV_SETS};
        const MaterialFromImportResult clamped = materialDocumentFromImported(atLimit, images);
        REQUIRE(documentSlotAt(clamped.document, 0).has_value());
        CHECK(documentSlotAt(clamped.document, 0)->uvSet == MATERIAL_MAX_UV_SETS - 1U);
        CHECK(clamped.warnings.size() == 1);
    }

    SUBCASE("one below it does not") {
        ImportedMaterial belowLimit;
        belowLimit.baseColor = ImportedTextureRef{.imageIndex = 0, .uvSet = MATERIAL_MAX_UV_SETS - 1U};
        const MaterialFromImportResult kept = materialDocumentFromImported(belowLimit, images);
        REQUIRE(documentSlotAt(kept.document, 0).has_value());
        CHECK(documentSlotAt(kept.document, 0)->uvSet == MATERIAL_MAX_UV_SETS - 1U);
        CHECK(kept.warnings.empty());
    }
}

TEST_CASE("material_from_import: warnings arrive in SLOT order (MF14)") {
    // Three different problems, on three different slots, deliberately declared out of slot order in
    // the source so only a slot-ordered walk produces this sequence.
    ImportedMaterial source;
    source.emissive = ImportedTextureRef{.imageIndex = 9};               // slot 4: missing image
    source.baseColor = ImportedTextureRef{.imageIndex = 0};              // slot 0: embedded
    source.normal = ImportedTextureRef{.imageIndex = 1};                 // slot 2: refused
    source.occlusion = ImportedTextureRef{.imageIndex = 2, .uvSet = 5};  // slot 3: clamped

    ImportedImage embedded;
    embedded.embedded = true;
    ImportedImage refused;
    refused.uri = "bad.png";
    refused.refusal = "a backslash is not a URI separator";
    const std::array<ImportedImage, 3> images{embedded, refused, resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    REQUIRE(result.warnings.size() == 4);
    CHECK(result.warnings[0].starts_with("baseColor: "));
    CHECK(result.warnings[1].starts_with("normal: "));
    CHECK(result.warnings[2].starts_with("occlusion: "));
    CHECK(result.warnings[3].starts_with("emissive: "));
}

TEST_CASE("material_from_import: EVERY omission arm leaves its slot DISENGAGED (MF15, S25)") {
    // ALL FOUR arms in one material, one per slot, so this is a real second witness for "omit, never
    // bind with a nil guid" rather than four repetitions of the first arm. An earlier draft engaged
    // every slot with an out-of-range index and therefore exercised arm 1 five times, which left the
    // embedded/refused/unresolved arms with MF8/MF9/MF10 as their only cover -- measured, not assumed:
    // seeding S25 (bind instead of omit, on the embedded arm) left that draft GREEN here.
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 99};         // arm 1: no such image
    source.metallicRoughness = ImportedTextureRef{.imageIndex = 0};  // arm 2: embedded
    source.normal = ImportedTextureRef{.imageIndex = 1};             // arm 3: refused
    source.occlusion = ImportedTextureRef{.imageIndex = 2};          // arm 4: resolved to no asset
    source.emissive = ImportedTextureRef{.imageIndex = 99};          // arm 1 again

    ImportedImage embedded;
    embedded.embedded = true;
    ImportedImage refused;
    refused.uri = "bad.png";
    refused.refusal = "a remote URI is never read";
    ImportedImage unresolved;
    unresolved.uri = "textures/gone.png";
    unresolved.relativePath = unresolved.uri;
    const std::array<ImportedImage, 3> images{embedded, refused, unresolved};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK(result.warnings.size() == engine::render::MATERIAL_TEXTURE_SLOT_COUNT);
    for (std::size_t slot = 0; slot < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
        CAPTURE(materialSlotLabel(slot));
        CHECK_FALSE(documentSlotAt(result.document, slot).has_value());
    }
    // and the whole document is still the DEFAULT one -- an omission changes nothing else
    CHECK(result.document == MaterialDocument{});
}

TEST_CASE("material_from_import: a material with no slots produces zero warnings (MF16)") {
    ImportedMaterial source;
    source.name = "Plain";
    source.metallicFactor = 0.0F;
    const std::array<ImportedImage, 1> images{resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK(result.warnings.empty());
    for (std::size_t slot = 0; slot < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
        CHECK_FALSE(documentSlotAt(result.document, slot).has_value());
    }
}

TEST_CASE("material_from_import: the produced document PASSES validateMaterial (MF17)") {
    // material_format exposes validateMaterial publicly, so this is the real predicate rather than a
    // stand-in: whatever this materializer emits must be a document saveMaterialFile would accept.
    ImportedMaterial source;
    source.name = "Full";
    source.baseColorFactor = Vec4{0.5F, 0.5F, 0.5F, 1.0F};
    source.metallicFactor = 0.4F;
    source.roughnessFactor = 0.6F;
    source.emissiveFactor = Vec3{0.0F, 0.25F, 0.0F};
    source.normalScale = 1.0F;
    source.occlusionStrength = 1.0F;
    source.alphaMode = AlphaMode::Blend;
    source.alphaCutoff = 0.5F;
    source.baseColor = ImportedTextureRef{.imageIndex = 0, .uvSet = 9};  // clamped on the way in
    source.normal = ImportedTextureRef{.imageIndex = 1};                 // omitted (embedded)

    ImportedImage embedded;
    embedded.embedded = true;
    const std::array<ImportedImage, 2> images{resolvedImage("wood.png", WOOD_GUID), embedded};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    CHECK_FALSE(validateMaterial(result.document).has_value());

    SUBCASE("and so does the default one") {
        CHECK_FALSE(validateMaterial(materialDocumentFromImported(ImportedMaterial{}, {}).document).has_value());
    }
}

TEST_CASE("material_from_import: the bound tokens compose the expected rhi::SamplerDesc LITERALS (MF18, S27)") {
    ImportedMaterial source;
    source.baseColor = ImportedTextureRef{.imageIndex = 0,
                                          .uvSet = 0,
                                          .wrapU = TextureWrap::ClampToEdge,
                                          .wrapV = TextureWrap::MirroredRepeat,
                                          .minFilter = TextureFilter::Nearest,
                                          .magFilter = TextureFilter::Linear,
                                          .mipFilter = MipFilter::None};
    const std::array<ImportedImage, 1> images{resolvedImage("wood.png", WOOD_GUID)};

    const MaterialFromImportResult result = materialDocumentFromImported(source, images);
    REQUIRE(documentSlotAt(result.document, 0).has_value());
    const engine::rhi::SamplerDesc desc = materialSamplerDescFor(*documentSlotAt(result.document, 0));

    // Against rhi LITERALS, never against materialAddressModeFor(...) -- the ME24 lesson: a
    // mapping-vs-mapping comparison cannot see a swapped clamp/mirror at all.
    CHECK((desc.addressU == engine::rhi::AddressMode::ClampToEdge));
    CHECK((desc.addressV == engine::rhi::AddressMode::MirroredRepeat));
    CHECK((desc.minFilter == engine::rhi::Filter::Nearest));
    CHECK((desc.magFilter == engine::rhi::Filter::Linear));
    // "none" is the clamp-to-base idiom: rhi::MipmapMode has no None, so it is Nearest AND maxLod 0.
    CHECK((desc.mipmapMode == engine::rhi::MipmapMode::Nearest));
    CHECK(desc.maxLod == doctest::Approx(0.0F));

    SUBCASE("a default-token slot composes the default sampler") {
        ImportedMaterial plain;
        plain.baseColor = ImportedTextureRef{.imageIndex = 0};
        const MaterialFromImportResult plainResult = materialDocumentFromImported(plain, images);
        REQUIRE(documentSlotAt(plainResult.document, 0).has_value());
        const engine::rhi::SamplerDesc plainDesc = materialSamplerDescFor(*documentSlotAt(plainResult.document, 0));
        CHECK((plainDesc.addressU == engine::rhi::AddressMode::Repeat));
        CHECK((plainDesc.addressV == engine::rhi::AddressMode::Repeat));
        CHECK((plainDesc.minFilter == engine::rhi::Filter::Linear));
        CHECK((plainDesc.magFilter == engine::rhi::Filter::Linear));
        CHECK((plainDesc.mipmapMode == engine::rhi::MipmapMode::Linear));
    }
}

TEST_CASE("material_from_import: materialSlotIsSrgb answers each slot, against literals (MF19, S24)") {
    // The colour-space half of a materialized slot, pinned HERE so the loader that will hand these
    // guids to the texture cook cannot hard-code `srgb = false` unnoticed. Literals, not a composition
    // of the function under test.
    CHECK(materialSlotIsSrgb(0));        // baseColor
    CHECK_FALSE(materialSlotIsSrgb(1));  // metallicRoughness
    CHECK_FALSE(materialSlotIsSrgb(2));  // normal
    CHECK_FALSE(materialSlotIsSrgb(3));  // occlusion
    CHECK(materialSlotIsSrgb(4));        // emissive

    std::size_t srgbSlots = 0;
    for (std::size_t slot = 0; slot < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
        srgbSlots += materialSlotIsSrgb(slot) ? 1U : 0U;
    }
    CHECK(srgbSlots == 2);
}

TEST_CASE("material_from_import: the name is carried VERBATIM, '' included (MF20)") {
    ImportedMaterial named;
    named.name = "  Wood ## Oak  ";
    CHECK(materialDocumentFromImported(named, {}).document.name == "  Wood ## Oak  ");

    const ImportedMaterial unnamed;
    CHECK(materialDocumentFromImported(unnamed, {}).document.name.empty());
}

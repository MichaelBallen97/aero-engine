// tests/editor/material_edit_test.cpp -- task 3.4.2, Step 1: the PURE document->render bridge
// (doc->params, the slot walk both ways, docs/09 section 11.4's token->SamplerDesc table, the four
// enum mirrors, and New Material's unique-name helper). A TU of aero_editor_shell_test, which
// supplies main() from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (the asset_view_test.cpp / mesh_cook_source_test.cpp precedent): material_edit.hpp depends
// on aero/reflect/material_format.hpp, aero/render/material.hpp and aero/rhi/descriptors.hpp, none of
// them gated -- aero::reflect arrives PUBLICly through aero::scene and aero::render joined
// aero_editor_core's PUBLIC group at this task. Every case here must therefore be PRESENT and PASSING
// in all three build configurations; prove it with --list-test-cases. Tier-0: no GPU, no window, no
// ImGui context, no disk I/O, no entropy source.
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md): MS STL defines
// operator<<(std::ostream&, std::string_view) inline in <string_view> against a basic_ostream only
// <iosfwd> has declared, so a CHECK that stringifies a string_view fails the Windows lane alone, with
// the error reported inside the STL headers. Four occurrences on record; this TU pays it up front.
//
// Enum CHECKs use the DOUBLE-PAREN posture -- CHECK((a == b)) -- which stops doctest's expression
// decomposition entirely. No toString overload is added anywhere: DOCTEST_STRINGIFY expands to an
// UNQUALIFIED toString(...), so an engine one is found by ADL and hard-errors every lane inside
// doctest.h. material_format.hpp's own header says the same thing about its material*Label naming.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>     // ME49: ImportChange -- the record's own "was this hashed?"
#include <aero/editor/asset_database.hpp>  // ME33+: the session takes the REAL database, by parameter
#include <aero/editor/asset_meta.hpp>      // ME49: AssetRecord + assetContentHashUsable
#include <aero/editor/material_edit.hpp>
#include <aero/editor/material_session.hpp>
#include <aero/editor/model_import.hpp>  // ME25-ME28: the editor value sets the four format enums mirror
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/types.hpp>

#include <doctest/doctest.h>

#include <algorithm>  // ME48: std::find over the key list
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>  // ME48: std::pair, the (guid, srgb) key half provable at this tier
#include <vector>

using engine::Guid;
using engine::MaterialAlphaMode;
using engine::MaterialDocument;
using engine::MaterialFilter;
using engine::MaterialMipFilter;
using engine::MaterialTextureSlot;
using engine::MaterialWrap;
using engine::Vec3;
using engine::Vec4;
using engine::editor::documentSlotAt;
using engine::editor::materialAddressModeFor;
using engine::editor::materialAlphaFor;
using engine::editor::materialFilterFor;
using engine::editor::materialParamsFor;
using engine::editor::materialSamplerDescFor;
using engine::editor::materialSlotIsSrgb;
using engine::editor::materialSlotLabel;
using engine::editor::MAX_NEW_MATERIAL_ATTEMPTS;
using engine::editor::uniqueMaterialFileName;

namespace {

// A document whose every scalar carries a DISTINCT value (the PB13 posture from task 3.4.1): no two
// fields share a number, so a crossed assignment inside materialParamsFor cannot look correct.
[[nodiscard]] MaterialDocument distinctDocument() {
    MaterialDocument doc;
    doc.name = "distinct";
    doc.baseColorFactor = Vec4{0.11F, 0.22F, 0.33F, 0.44F};
    doc.emissiveFactor = Vec3{0.55F, 0.66F, 0.77F};
    doc.metallicFactor = 0.125F;
    doc.roughnessFactor = 0.375F;
    doc.normalScale = 1.75F;
    doc.occlusionStrength = 0.625F;
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.alphaCutoff = 0.875F;
    doc.doubleSided = true;
    return doc;
}

[[nodiscard]] MaterialTextureSlot slotWithGuid(std::uint64_t lo) {
    MaterialTextureSlot slot;
    slot.guid = Guid{.hi = 0, .lo = lo};
    return slot;
}

constexpr std::array<MaterialAlphaMode, 3> ALPHA_MODES{MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask,
                                                       MaterialAlphaMode::Blend};
constexpr std::array<MaterialWrap, 3> WRAPS{MaterialWrap::Repeat, MaterialWrap::Clamp, MaterialWrap::Mirror};
constexpr std::array<MaterialFilter, 2> FILTERS{MaterialFilter::Nearest, MaterialFilter::Linear};
constexpr std::array<MaterialMipFilter, 3> MIP_FILTERS{MaterialMipFilter::None, MaterialMipFilter::Nearest,
                                                       MaterialMipFilter::Linear};

}  // namespace

// ---- doc -> params (ME1-ME8) --------------------------------------------------------------------

TEST_CASE("material edit: every field maps across with mutually distinct values (ME1, AC-26)") {
    const MaterialDocument doc = distinctDocument();
    const engine::render::MaterialParams params = materialParamsFor(doc);
    CHECK(params.baseColorFactor.x == doctest::Approx(0.11F));
    CHECK(params.baseColorFactor.y == doctest::Approx(0.22F));
    CHECK(params.baseColorFactor.z == doctest::Approx(0.33F));
    CHECK(params.baseColorFactor.w == doctest::Approx(0.44F));
    CHECK(params.emissiveFactor.x == doctest::Approx(0.55F));
    CHECK(params.emissiveFactor.y == doctest::Approx(0.66F));
    CHECK(params.emissiveFactor.z == doctest::Approx(0.77F));
    CHECK(params.metallicFactor == doctest::Approx(0.125F));
    CHECK(params.roughnessFactor == doctest::Approx(0.375F));
    CHECK(params.normalScale == doctest::Approx(1.75F));
    CHECK(params.occlusionStrength == doctest::Approx(0.625F));
    CHECK((params.alpha == engine::render::MaterialAlpha::Mask));
    CHECK(params.alphaCutoff == doctest::Approx(0.875F));
    CHECK(params.doubleSided);
}

TEST_CASE("material edit: alpha mode opaque maps to MaterialAlpha::Opaque (ME2)") {
    MaterialDocument doc;
    doc.alphaMode = MaterialAlphaMode::Opaque;
    CHECK((materialParamsFor(doc).alpha == engine::render::MaterialAlpha::Opaque));
    CHECK((materialAlphaFor(MaterialAlphaMode::Opaque) == engine::render::MaterialAlpha::Opaque));
}

TEST_CASE("material edit: alpha mode mask maps to MaterialAlpha::Mask (ME3)") {
    MaterialDocument doc;
    doc.alphaMode = MaterialAlphaMode::Mask;
    CHECK((materialParamsFor(doc).alpha == engine::render::MaterialAlpha::Mask));
    CHECK((materialAlphaFor(MaterialAlphaMode::Mask) == engine::render::MaterialAlpha::Mask));
}

TEST_CASE("material edit: alpha mode blend maps to MaterialAlpha::Blend (ME4)") {
    MaterialDocument doc;
    doc.alphaMode = MaterialAlphaMode::Blend;
    CHECK((materialParamsFor(doc).alpha == engine::render::MaterialAlpha::Blend));
    CHECK((materialAlphaFor(MaterialAlphaMode::Blend) == engine::render::MaterialAlpha::Blend));
}

TEST_CASE("material edit: a default document maps to the FORMAT's defaults, not the renderer's (ME5)") {
    // Two different questions, and material.hpp says so at DEFAULT_MATERIAL_PARAMS' own declaration:
    // MaterialParams{} is glTF's / the file format's default set (metallic 1), while
    // DEFAULT_MATERIAL_PARAMS is what an UNSET MeshInstance::material draws with (metallic 0).
    const engine::render::MaterialParams params = materialParamsFor(MaterialDocument{});
    CHECK((params == engine::render::MaterialParams{}));
    CHECK(params.metallicFactor == doctest::Approx(1.0F));
    CHECK_FALSE((params == engine::render::DEFAULT_MATERIAL_PARAMS));
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.metallicFactor == doctest::Approx(0.0F));
}

TEST_CASE("material edit: doubleSided survives both ways (ME6)") {
    MaterialDocument doc;
    doc.doubleSided = false;
    CHECK_FALSE(materialParamsFor(doc).doubleSided);
    doc.doubleSided = true;
    CHECK(materialParamsFor(doc).doubleSided);
}

TEST_CASE("material edit: alphaCutoff survives EVERY alpha mode (ME7)") {
    // The file stores the cutoff regardless of the mode (docs/09 section 11), and the panel hides the
    // ROW rather than resetting the value -- so the bridge must never zero it for Opaque or Blend.
    for (const MaterialAlphaMode mode : ALPHA_MODES) {
        MaterialDocument doc;
        doc.alphaMode = mode;
        doc.alphaCutoff = 0.3125F;
        CAPTURE(static_cast<int>(mode));
        CHECK(materialParamsFor(doc).alphaCutoff == doctest::Approx(0.3125F));
    }
}

TEST_CASE("material edit: materialAlphaFor is total over MaterialAlphaMode (ME8)") {
    constexpr std::array<engine::render::MaterialAlpha, 3> EXPECTED{engine::render::MaterialAlpha::Opaque,
                                                                    engine::render::MaterialAlpha::Mask,
                                                                    engine::render::MaterialAlpha::Blend};
    REQUIRE(ALPHA_MODES.size() == EXPECTED.size());
    for (std::size_t i = 0; i < ALPHA_MODES.size(); ++i) {
        CAPTURE(i);
        CHECK((materialAlphaFor(ALPHA_MODES[i]) == EXPECTED[i]));
    }
}

// ---- the slot walk (ME9-ME14) --------------------------------------------------------------------

TEST_CASE("material edit: the const slot walk answers all five indices (ME9)") {
    MaterialDocument doc;
    doc.baseColor = slotWithGuid(10);
    doc.metallicRoughness = slotWithGuid(11);
    doc.normal = slotWithGuid(12);
    doc.occlusion = slotWithGuid(13);
    doc.emissive = slotWithGuid(14);
    const MaterialDocument& constDoc = doc;
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        CAPTURE(i);
        const std::optional<MaterialTextureSlot>& slot = documentSlotAt(constDoc, i);
        REQUIRE(slot.has_value());
        CHECK(slot->guid.lo == 10U + i);
    }
}

TEST_CASE("material edit: the non-const slot walk WRITES the slot the index names (ME10)") {
    MaterialDocument doc;
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        documentSlotAt(doc, i) = slotWithGuid(20 + i);
    }
    REQUIRE(doc.baseColor.has_value());
    REQUIRE(doc.metallicRoughness.has_value());
    REQUIRE(doc.normal.has_value());
    REQUIRE(doc.occlusion.has_value());
    REQUIRE(doc.emissive.has_value());
    CHECK(doc.baseColor->guid.lo == 20U);
    CHECK(doc.metallicRoughness->guid.lo == 21U);
    CHECK(doc.normal->guid.lo == 22U);
    CHECK(doc.occlusion->guid.lo == 23U);
    CHECK(doc.emissive->guid.lo == 24U);
}

TEST_CASE("material edit: both slot-walk directions agree, index by index (ME11, seed S17)") {
    // The seed swaps normal and occlusion. Naming BOTH members explicitly beside their index is what
    // makes that a red line rather than a differently-lit preview nobody can prove is wrong.
    MaterialDocument doc;
    doc.baseColor = slotWithGuid(1);
    doc.metallicRoughness = slotWithGuid(2);
    doc.normal = slotWithGuid(3);
    doc.occlusion = slotWithGuid(4);
    doc.emissive = slotWithGuid(5);
    const MaterialDocument& constDoc = doc;
    CHECK(documentSlotAt(constDoc, 0)->guid == doc.baseColor->guid);
    CHECK(documentSlotAt(constDoc, 1)->guid == doc.metallicRoughness->guid);
    CHECK(documentSlotAt(constDoc, 2)->guid == doc.normal->guid);
    CHECK(documentSlotAt(constDoc, 3)->guid == doc.occlusion->guid);
    CHECK(documentSlotAt(constDoc, 4)->guid == doc.emissive->guid);
    // ... and back through the writer, which must land on the SAME five members.
    MaterialDocument rebuilt;
    documentSlotAt(rebuilt, 2) = slotWithGuid(3);
    documentSlotAt(rebuilt, 3) = slotWithGuid(4);
    CHECK_FALSE(rebuilt.baseColor.has_value());
    CHECK_FALSE(rebuilt.metallicRoughness.has_value());
    CHECK_FALSE(rebuilt.emissive.has_value());
    REQUIRE(rebuilt.normal.has_value());
    REQUIRE(rebuilt.occlusion.has_value());
    CHECK(rebuilt.normal->guid.lo == 3U);
    CHECK(rebuilt.occlusion->guid.lo == 4U);
}

TEST_CASE("material edit: a disengaged slot reads as nullopt at its own index (ME12)") {
    MaterialDocument doc;
    doc.normal = slotWithGuid(7);
    const MaterialDocument& constDoc = doc;
    CHECK_FALSE(documentSlotAt(constDoc, 0).has_value());
    CHECK_FALSE(documentSlotAt(constDoc, 1).has_value());
    CHECK(documentSlotAt(constDoc, 2).has_value());
    CHECK_FALSE(documentSlotAt(constDoc, 3).has_value());
    CHECK_FALSE(documentSlotAt(constDoc, 4).has_value());
}

TEST_CASE("material edit: materialSlotLabel is total, non-empty and distinct over 0..4 (ME13)") {
    std::vector<std::string_view> labels;
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        CAPTURE(i);
        const std::string_view label = materialSlotLabel(i);
        CHECK_FALSE(label.empty());
        labels.push_back(label);
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        for (std::size_t j = i + 1; j < labels.size(); ++j) {
            CAPTURE(i);
            CAPTURE(j);
            CHECK(labels[i] != labels[j]);
        }
    }
    CHECK(materialSlotLabel(0) == std::string_view("baseColor"));
    CHECK(materialSlotLabel(4) == std::string_view("emissive"));
}

TEST_CASE("material edit: the document index order EQUALS render::materialSlotAt's (ME14)") {
    // Walk BOTH orders and cross them. The document side numbers its slots through guid.lo; the
    // render side numbers the same five, in ITS declaration order, through maxLod. If either walk's
    // order moves, the two disagree at the moved index.
    MaterialDocument doc;
    doc.baseColor = slotWithGuid(10);
    doc.metallicRoughness = slotWithGuid(11);
    doc.normal = slotWithGuid(12);
    doc.occlusion = slotWithGuid(13);
    doc.emissive = slotWithGuid(14);

    engine::render::MaterialTextureSlots renderSlots;
    renderSlots.baseColor.sampler.maxLod = 10.0F;
    renderSlots.metallicRoughness.sampler.maxLod = 11.0F;
    renderSlots.normal.sampler.maxLod = 12.0F;
    renderSlots.occlusion.sampler.maxLod = 13.0F;
    renderSlots.emissive.sampler.maxLod = 14.0F;

    const MaterialDocument& constDoc = doc;
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        CAPTURE(i);
        const std::optional<MaterialTextureSlot>& docSlot = documentSlotAt(constDoc, i);
        REQUIRE(docSlot.has_value());
        const engine::render::MaterialTextureSlot& renderSlot = engine::render::materialSlotAt(renderSlots, i);
        CHECK(static_cast<float>(docSlot->guid.lo) == doctest::Approx(renderSlot.sampler.maxLod));
    }
}

TEST_CASE("material edit: the per-slot colour space mirrors defaultTextureKindForSlot (ME14b, D7)") {
    // COMPOSED from that function rather than restated beside it -- 3.4.1 deleted a hand-written
    // five-entry table for exactly this reason, and its own comment says a wrong default still draws
    // a fully lit surface, so there is no visual witness at all.
    CHECK(materialSlotIsSrgb(0));        // baseColor
    CHECK_FALSE(materialSlotIsSrgb(1));  // metallicRoughness
    CHECK_FALSE(materialSlotIsSrgb(2));  // normal
    CHECK_FALSE(materialSlotIsSrgb(3));  // occlusion
    CHECK(materialSlotIsSrgb(4));        // emissive
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        CAPTURE(i);
        const bool viaKind =
            engine::render::defaultTextureKindForSlot(i) == engine::render::MaterialDefaultTextureKind::WhiteSrgb;
        CHECK(materialSlotIsSrgb(i) == viaKind);
    }
}

// ---- docs/09 section 11.4 totality (ME15-ME24) ---------------------------------------------------

TEST_CASE("material edit: wrap 'repeat' maps to AddressMode::Repeat (ME15)") {
    CHECK((materialAddressModeFor(MaterialWrap::Repeat) == engine::rhi::AddressMode::Repeat));
}

TEST_CASE("material edit: wrap 'clamp' maps to AddressMode::ClampToEdge (ME16)") {
    CHECK((materialAddressModeFor(MaterialWrap::Clamp) == engine::rhi::AddressMode::ClampToEdge));
}

TEST_CASE("material edit: wrap 'mirror' maps to AddressMode::MirroredRepeat (ME17)") {
    CHECK((materialAddressModeFor(MaterialWrap::Mirror) == engine::rhi::AddressMode::MirroredRepeat));
}

TEST_CASE("material edit: filter 'nearest' maps to Filter::Nearest (ME18)") {
    CHECK((materialFilterFor(MaterialFilter::Nearest) == engine::rhi::Filter::Nearest));
}

TEST_CASE("material edit: filter 'linear' maps to Filter::Linear (ME19)") {
    CHECK((materialFilterFor(MaterialFilter::Linear) == engine::rhi::Filter::Linear));
}

TEST_CASE("material edit: mipFilter nearest/linear leave maxLod at the desc DEFAULT (ME20)") {
    const engine::rhi::SamplerDesc defaultDesc{};
    MaterialTextureSlot nearest;
    nearest.mipFilter = MaterialMipFilter::Nearest;
    const engine::rhi::SamplerDesc nearestDesc = materialSamplerDescFor(nearest);
    CHECK((nearestDesc.mipmapMode == engine::rhi::MipmapMode::Nearest));
    CHECK(nearestDesc.maxLod == doctest::Approx(defaultDesc.maxLod));

    MaterialTextureSlot linear;
    linear.mipFilter = MaterialMipFilter::Linear;
    const engine::rhi::SamplerDesc linearDesc = materialSamplerDescFor(linear);
    CHECK((linearDesc.mipmapMode == engine::rhi::MipmapMode::Linear));
    CHECK(linearDesc.maxLod == doctest::Approx(defaultDesc.maxLod));

    // The value the desc default actually is, pinned once: "effectively unclamped", the Vulkan
    // LOD_CLAMP_NONE idiom. ME21's maxLod-0 assertion means nothing without this number beside it.
    CHECK(defaultDesc.maxLod == doctest::Approx(1000.0F));
}

TEST_CASE("material edit: mipFilter 'none' is Nearest AND maxLod 0 -- both halves (ME21, seed S14)") {
    MaterialTextureSlot slot;
    slot.mipFilter = MaterialMipFilter::None;
    const engine::rhi::SamplerDesc desc = materialSamplerDescFor(slot);
    CHECK((desc.mipmapMode == engine::rhi::MipmapMode::Nearest));
    CHECK(desc.maxLod == doctest::Approx(0.0F));
}

TEST_CASE("material edit: wrapU and wrapV map INDEPENDENTLY (ME22, seed S13)") {
    MaterialTextureSlot slot;
    slot.wrapU = MaterialWrap::Clamp;
    slot.wrapV = MaterialWrap::Mirror;
    const engine::rhi::SamplerDesc desc = materialSamplerDescFor(slot);
    CHECK((desc.addressU == engine::rhi::AddressMode::ClampToEdge));
    CHECK((desc.addressV == engine::rhi::AddressMode::MirroredRepeat));
    // ... and the other way round, so a mapping that read one field twice fails one of the two.
    MaterialTextureSlot swapped;
    swapped.wrapU = MaterialWrap::Mirror;
    swapped.wrapV = MaterialWrap::Clamp;
    const engine::rhi::SamplerDesc swappedDesc = materialSamplerDescFor(swapped);
    CHECK((swappedDesc.addressU == engine::rhi::AddressMode::MirroredRepeat));
    CHECK((swappedDesc.addressV == engine::rhi::AddressMode::ClampToEdge));
}

TEST_CASE("material edit: addressW is UNTOUCHED at the desc default (ME23, section 11.4)") {
    const engine::rhi::SamplerDesc defaultDesc{};
    for (const MaterialWrap wrap : WRAPS) {
        MaterialTextureSlot slot;
        slot.wrapU = wrap;
        slot.wrapV = wrap;
        CAPTURE(static_cast<int>(wrap));
        CHECK((materialSamplerDescFor(slot).addressW == defaultDesc.addressW));
    }
}

TEST_CASE("material edit: the full wrap x min x mag x mip cross product, 36 combinations (ME24, AC-26)") {
    const engine::rhi::SamplerDesc defaultDesc{};
    std::size_t combinations = 0;
    for (const MaterialWrap wrap : WRAPS) {
        for (const MaterialFilter minFilter : FILTERS) {
            for (const MaterialFilter magFilter : FILTERS) {
                for (const MaterialMipFilter mipFilter : MIP_FILTERS) {
                    MaterialTextureSlot slot;
                    slot.wrapU = wrap;
                    slot.wrapV = wrap;
                    slot.minFilter = minFilter;
                    slot.magFilter = magFilter;
                    slot.mipFilter = mipFilter;
                    const engine::rhi::SamplerDesc desc = materialSamplerDescFor(slot);
                    CAPTURE(static_cast<int>(wrap));
                    CAPTURE(static_cast<int>(minFilter));
                    CAPTURE(static_cast<int>(magFilter));
                    CAPTURE(static_cast<int>(mipFilter));
                    CHECK((desc.addressU == materialAddressModeFor(wrap)));
                    CHECK((desc.addressV == materialAddressModeFor(wrap)));
                    CHECK((desc.addressW == defaultDesc.addressW));
                    CHECK((desc.minFilter == materialFilterFor(minFilter)));
                    CHECK((desc.magFilter == materialFilterFor(magFilter)));
                    const bool wantsNearestMips = mipFilter != MaterialMipFilter::Linear;
                    CHECK((desc.mipmapMode ==
                           (wantsNearestMips ? engine::rhi::MipmapMode::Nearest : engine::rhi::MipmapMode::Linear)));
                    const float expectedMaxLod = mipFilter == MaterialMipFilter::None ? 0.0F : defaultDesc.maxLod;
                    CHECK(desc.maxLod == doctest::Approx(expectedMaxLod));
                    ++combinations;
                }
            }
        }
    }
    CHECK(combinations == 36U);
}

// ---- the four enum mirrors (ME25-ME28) -- 3.4.1's named obligation, discharged --------------------

TEST_CASE("material edit: MaterialAlphaMode mirrors the importer's AlphaMode 1:1 (ME25, AC-27)") {
    // material_format.hpp:27-30 assigned this assertion to THIS task and THIS tier by name. The
    // correspondence is by VALUE, both directions, so a future import-materializer is lossless.
    struct Pair {
        MaterialAlphaMode format;
        engine::editor::AlphaMode editor;
    };
    constexpr std::array<Pair, 3> PAIRS{Pair{MaterialAlphaMode::Opaque, engine::editor::AlphaMode::Opaque},
                                        Pair{MaterialAlphaMode::Mask, engine::editor::AlphaMode::Mask},
                                        Pair{MaterialAlphaMode::Blend, engine::editor::AlphaMode::Blend}};
    REQUIRE(PAIRS.size() == ALPHA_MODES.size());  // a dropped row is a red line, not a shorter loop
    for (const Pair& p : PAIRS) {
        CAPTURE(static_cast<int>(p.format));
        CHECK(static_cast<int>(p.format) == static_cast<int>(p.editor));
        CHECK((static_cast<MaterialAlphaMode>(static_cast<int>(p.editor)) == p.format));
        CHECK((static_cast<engine::editor::AlphaMode>(static_cast<int>(p.format)) == p.editor));
    }
}

TEST_CASE("material edit: MaterialWrap mirrors the importer's TextureWrap 1:1 (ME26, AC-27)") {
    // Two of the three spellings DIFFER -- Clamp/ClampToEdge and Mirror/MirroredRepeat -- which is
    // precisely why the correspondence needs asserting rather than eyeballing.
    struct Pair {
        MaterialWrap format;
        engine::editor::TextureWrap editor;
    };
    constexpr std::array<Pair, 3> PAIRS{Pair{MaterialWrap::Repeat, engine::editor::TextureWrap::Repeat},
                                        Pair{MaterialWrap::Clamp, engine::editor::TextureWrap::ClampToEdge},
                                        Pair{MaterialWrap::Mirror, engine::editor::TextureWrap::MirroredRepeat}};
    REQUIRE(PAIRS.size() == WRAPS.size());
    for (const Pair& p : PAIRS) {
        CAPTURE(static_cast<int>(p.format));
        CHECK(static_cast<int>(p.format) == static_cast<int>(p.editor));
        CHECK((static_cast<MaterialWrap>(static_cast<int>(p.editor)) == p.format));
        CHECK((static_cast<engine::editor::TextureWrap>(static_cast<int>(p.format)) == p.editor));
    }
}

TEST_CASE("material edit: MaterialFilter mirrors the importer's TextureFilter 1:1 (ME27, AC-27)") {
    struct Pair {
        MaterialFilter format;
        engine::editor::TextureFilter editor;
    };
    constexpr std::array<Pair, 2> PAIRS{Pair{MaterialFilter::Nearest, engine::editor::TextureFilter::Nearest},
                                        Pair{MaterialFilter::Linear, engine::editor::TextureFilter::Linear}};
    REQUIRE(PAIRS.size() == FILTERS.size());
    for (const Pair& p : PAIRS) {
        CAPTURE(static_cast<int>(p.format));
        CHECK(static_cast<int>(p.format) == static_cast<int>(p.editor));
        CHECK((static_cast<MaterialFilter>(static_cast<int>(p.editor)) == p.format));
        CHECK((static_cast<engine::editor::TextureFilter>(static_cast<int>(p.format)) == p.editor));
    }
}

TEST_CASE("material edit: MaterialMipFilter mirrors the importer's MipFilter 1:1 (ME28, AC-27)") {
    struct Pair {
        MaterialMipFilter format;
        engine::editor::MipFilter editor;
    };
    constexpr std::array<Pair, 3> PAIRS{Pair{MaterialMipFilter::None, engine::editor::MipFilter::None},
                                        Pair{MaterialMipFilter::Nearest, engine::editor::MipFilter::Nearest},
                                        Pair{MaterialMipFilter::Linear, engine::editor::MipFilter::Linear}};
    REQUIRE(PAIRS.size() == MIP_FILTERS.size());
    for (const Pair& p : PAIRS) {
        CAPTURE(static_cast<int>(p.format));
        CHECK(static_cast<int>(p.format) == static_cast<int>(p.editor));
        CHECK((static_cast<MaterialMipFilter>(static_cast<int>(p.editor)) == p.format));
        CHECK((static_cast<engine::editor::MipFilter>(static_cast<int>(p.format)) == p.editor));
    }
}

// ---- New Material's unique name (ME29-ME32) ------------------------------------------------------

TEST_CASE("material edit: an empty directory yields the bare name (ME29, AC-5)") {
    const std::vector<std::string_view> taken;
    CHECK(uniqueMaterialFileName("NewMaterial", taken) == std::string("NewMaterial.aeromat"));
}

TEST_CASE("material edit: one collision counts to -2 (ME30, AC-5)") {
    const std::array<std::string_view, 1> taken{"NewMaterial.aeromat"};
    CHECK(uniqueMaterialFileName("NewMaterial", taken) == std::string("NewMaterial-2.aeromat"));
}

TEST_CASE("material edit: a run of collisions counts up, ASCII-case-insensitively (ME31, AC-5)") {
    // Two names differing only in case COLLIDE on a case-insensitive filesystem, so the comparison is
    // folded -- the extensionEqualsFolded posture, one predicate over.
    const std::array<std::string_view, 4> taken{"NEWMATERIAL.AEROMAT", "NewMaterial-2.aeromat", "newmaterial-3.AeroMat",
                                                "NewMaterial-4.aeromat"};
    CHECK(uniqueMaterialFileName("NewMaterial", taken) == std::string("NewMaterial-5.aeromat"));
    // Unrelated names never consume a suffix.
    const std::array<std::string_view, 2> unrelated{"wood.png", "OtherMaterial.aeromat"};
    CHECK(uniqueMaterialFileName("NewMaterial", unrelated) == std::string("NewMaterial.aeromat"));
}

TEST_CASE("material edit: exhaustion returns \"\" and never tries a 65th name (ME32, AC-6)") {
    std::vector<std::string> owned;
    owned.reserve(MAX_NEW_MATERIAL_ATTEMPTS);
    owned.emplace_back("NewMaterial.aeromat");
    for (std::size_t n = 2; n <= MAX_NEW_MATERIAL_ATTEMPTS; ++n) {
        owned.push_back("NewMaterial-" + std::to_string(n) + ".aeromat");
    }
    REQUIRE(owned.size() == MAX_NEW_MATERIAL_ATTEMPTS);
    std::vector<std::string_view> taken;
    taken.reserve(owned.size());
    for (const std::string& name : owned) {
        taken.emplace_back(name);
    }
    CHECK(uniqueMaterialFileName("NewMaterial", taken).empty());
    // A smaller budget exhausts sooner, and the bound is the PARAMETER, never a hidden constant.
    const std::array<std::string_view, 1> one{"NewMaterial.aeromat"};
    CHECK(uniqueMaterialFileName("NewMaterial", one, 1).empty());
    CHECK(uniqueMaterialFileName("NewMaterial", one, 2) == std::string("NewMaterial-2.aeromat"));
}

// ==== the session (ME33-ME46) =====================================================================
// Driven against a REAL AssetDatabase over a scratch tree -- the asset_database_test.cpp precedent,
// and house style throughout this repository: the real type, never a mock. Still tier-0: no GPU, no
// window, no ImGui context, and bounded disk I/O through a TempDir that removes itself.

namespace {

// A unique temp directory that removes itself on destruction -- the same TU-local shape
// asset_database_test.cpp carries (scaffolding is copied; the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter suffices
        dirPath = base / ("aero_material_session_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

private:
    std::filesystem::path dirPath;
};

// A path built from UTF-8 BYTES, never from a narrow std::string: filesystem::path's narrow-char
// constructor assumes the ACTIVE CODE PAGE on Windows (asset_database_test.cpp's AD30 lesson).
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

[[nodiscard]] std::string readBytes(std::string_view absolutePath) {
    const std::ifstream in(pathOf(absolutePath), std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// A committed fixture, by leaf name. A PATH, not a flag: a missing file is a REQUIRE failure here,
// never a silent skip.
[[nodiscard]] std::string fixtureText(std::string_view leaf) {
    std::string path = AERO_MATERIAL_FIXTURES_DIR;
    path += '/';
    path += leaf;
    const std::string text = readBytes(path);
    REQUIRE_MESSAGE(!text.empty(), "missing material fixture: ", path);
    return text;
}

// A project root + assets root + a real AssetDatabase over it.
class SessionHarness {
public:
    SessionHarness() {
        assetsRootValue = dir.utf8() + "/assets";
        std::error_code ec;
        std::filesystem::create_directories(pathOf(assetsRootValue), ec);
    }

    void writeAsset(std::string_view relativePath, std::string_view bytes) const {
        std::ofstream out(pathOf(absolutePathOf(relativePath)), std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void removeAsset(std::string_view relativePath) const {
        std::error_code ec;
        std::filesystem::remove(pathOf(absolutePathOf(relativePath)), ec);
        std::filesystem::remove(pathOf(absolutePathOf(relativePath) + ".meta"), ec);
    }

    void rescan() { (void)database.rescan(dir.utf8(), assetsRootValue, guids); }

    // ME49: the SAME scan under a starved per-scan hash budget, which is how a record legitimately ends
    // up NotHashed -- and therefore carrying an all-zero contentHash that means nothing (AD55's lever,
    // borrowed rather than re-invented).
    void rescanWithHashBudget(std::uint64_t bytes) {
        // invalidateCache FIRST, or there is nothing for the budget to starve: an unchanged file takes
        // the cache's (size, mtime) fast path and is never hashed at all, so its record keeps the
        // CACHED digest and reads as UpToDate. Discarding the index is what makes every file need a
        // hash again -- Reimport All's own lever (AC-39), used here to reach NotHashed deliberately.
        database.invalidateCache();
        (void)database.rescan(dir.utf8(), assetsRootValue, guids, bytes);
    }

    [[nodiscard]] std::string absolutePathOf(std::string_view relativePath) const {
        return assetsRootValue + "/" + std::string(relativePath);
    }
    [[nodiscard]] const engine::editor::AssetDatabase& db() const noexcept { return database; }
    [[nodiscard]] const std::string& assetsRoot() const noexcept { return assetsRootValue; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return database.generation(); }

private:
    TempDir dir;
    std::string assetsRootValue;
    engine::editor::AssetDatabase database;
    engine::GuidGenerator guids{0x3442U};
};

}  // namespace

TEST_CASE("material session: a DIFFERENT existing .aeromat retargets (ME33, AC-7)") {
    SessionHarness harness;
    harness.writeAsset("a.aeromat", fixtureText("canonical.aeromat"));
    harness.writeAsset("b.aeromat", fixtureText("defaulted.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("a.aeromat"));
    CHECK((session.state() == engine::editor::MaterialSessionState::Ready));
    REQUIRE(session.document() != nullptr);
    CHECK(session.document()->name == "Brushed Copper");

    session.reconcile("b.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("b.aeromat"));
    REQUIRE(session.document() != nullptr);
    CHECK((*session.document() == MaterialDocument{}));  // defaulted.aeromat is exactly {"version": 1}
    CHECK_FALSE(session.dirty());
}

TEST_CASE("material session: a NON-material selection leaves the target alone (ME34, D3, seed S3)") {
    // The sticky rule's whole point: finding a texture to reference means clicking through the
    // browser, and the browser has ONE selection. Retargeting on every selection would make every
    // such click tear down the edit session.
    SessionHarness harness;
    harness.writeAsset("a.aeromat", fixtureText("canonical.aeromat"));
    harness.writeAsset("wood.png", "not really a png, but the browser only reads the extension");
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    MaterialDocument edited = *session.document();
    edited.roughnessFactor = 0.125F;
    session.edit(edited);
    REQUIRE(session.dirty());

    session.reconcile("wood.png", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("a.aeromat"));
    CHECK(session.dirty());  // the unapplied edit SURVIVES the click
    REQUIRE(session.document() != nullptr);
    CHECK(session.document()->roughnessFactor == doctest::Approx(0.125F));

    // A FOLDER selection is the same answer.
    session.reconcile("", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("a.aeromat"));
}

TEST_CASE("material session: an EMPTY selection leaves the target alone (ME35, D3, seed S3)") {
    SessionHarness harness;
    harness.writeAsset("a.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE(session.targetPath() == std::string_view("a.aeromat"));
    session.reconcile("", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("a.aeromat"));
    CHECK((session.state() == engine::editor::MaterialSessionState::Ready));
    // ... and a material that does NOT exist in the database is not a target either.
    session.reconcile("ghost.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("a.aeromat"));
}

TEST_CASE("material session: re-selecting the SAME path is a no-op (ME36, AC-7)") {
    SessionHarness harness;
    harness.writeAsset("a.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    MaterialDocument edited = *session.document();
    edited.metallicFactor = 0.0F;
    session.edit(edited);
    REQUIRE(session.dirty());
    CHECK(session.takeDocumentChanged());  // drained, so a stale flag cannot fake the assertion below

    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.dirty());                      // NOT reloaded -- the edit is still here
    CHECK_FALSE(session.takeDocumentChanged());  // and nothing re-pushed to the preview
    CHECK(session.document()->metallicFactor == doctest::Approx(0.0F));
}

TEST_CASE("material session: an UPPER-CASE .AEROMAT retargets -- the extension is folded (ME37)") {
    SessionHarness harness;
    harness.writeAsset("Shiny.AEROMAT", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("Shiny.AEROMAT", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.targetPath() == std::string_view("Shiny.AEROMAT"));
    CHECK((session.state() == engine::editor::MaterialSessionState::Ready));
}

TEST_CASE("material session: a valid but NON-CANONICAL file loads CLEAN (ME38, D5, seed S4)") {
    // Dirty is sessionCopy != fileCopy through the DEFAULTED ==, never "would Apply change the
    // bytes". Reordered keys, an upper-case GUID and an unknown key all parse; none of them is an
    // edit, so Apply stays disabled and the editor never rewrites a file nobody touched.
    SessionHarness harness;
    const std::string source = fixtureText("noncanonical.aeromat");
    harness.writeAsset("odd.aeromat", source);
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("odd.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE((session.state() == engine::editor::MaterialSessionState::Ready));
    CHECK_FALSE(session.dirty());
    // The upper-case GUID was accepted (the tolerant-read / lowercase-write rule) ...
    REQUIRE(session.document() != nullptr);
    REQUIRE(session.document()->baseColor.has_value());
    CHECK(engine::formatGuid(session.document()->baseColor->guid) == std::string("1111111111111111aaaaaaaaaaaaaaaa"));
    // ... and the panel is handed the parser's OWN per-key list, naming the one key Apply will DELETE.
    // The reordered keys and the upper-case GUID are cosmetic and produce no warning at all: that
    // distinction is the whole reason the engine carries this channel instead of the editor guessing
    // at it from a byte comparison, which could only ever say "something here is not canonical".
    REQUIRE(session.warnings().size() == 1U);
    CHECK(session.warnings()[0] == std::string(R"(ignoring unknown key "authoredBy")"));
    // Nothing was written by merely LOOKING at it.
    CHECK(session.writeCount() == 0U);
    CHECK(readBytes(harness.absolutePathOf("odd.aeromat")) == source);
}

TEST_CASE("material session: Apply on a CLEAN session writes nothing (ME39, AC-12, seed S6)") {
    SessionHarness harness;
    const std::string source = fixtureText("canonical.aeromat");
    harness.writeAsset("a.aeromat", source);
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE_FALSE(session.dirty());
    session.requestApply();
    session.service(harness.db(), harness.assetsRoot());
    CHECK(session.writeCount() == 0U);
    CHECK(readBytes(harness.absolutePathOf("a.aeromat")) == source);
}

TEST_CASE("material session: Apply on a DIRTY session writes ONCE, canonically (ME40, seed S7)") {
    SessionHarness harness;
    harness.writeAsset("a.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    MaterialDocument edited = *session.document();
    edited.roughnessFactor = 0.75F;
    edited.name = "Edited";
    session.edit(edited);
    REQUIRE(session.dirty());

    session.requestApply();
    session.service(harness.db(), harness.assetsRoot());
    CHECK(session.writeCount() == 1U);
    CHECK_FALSE(session.dirty());  // the file copy ADOPTED the session copy
    // Byte for byte the canonical writer's own output -- S7's witness, and the fixpoint.
    CHECK(readBytes(harness.absolutePathOf("a.aeromat")) == engine::writeMaterialText(edited));

    // A SECOND Apply with nothing further edited writes nothing more.
    session.requestApply();
    session.service(harness.db(), harness.assetsRoot());
    CHECK(session.writeCount() == 1U);
}

TEST_CASE("material session: Apply REFUSES a NaN smuggled in through C++ (ME41, INV-7, seed S5)") {
    // validateMaterial reaches the arm a file cannot: a NaN factor is unspellable in JSON and
    // trivially assignable in C++, and every range check is written so NaN fails it. A validation
    // failure changes NOTHING anywhere.
    SessionHarness harness;
    const std::string source = fixtureText("canonical.aeromat");
    harness.writeAsset("a.aeromat", source);
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    MaterialDocument poisoned = *session.document();
    poisoned.roughnessFactor = std::numeric_limits<float>::quiet_NaN();
    session.edit(poisoned);
    REQUIRE(session.dirty());

    session.requestApply();
    session.service(harness.db(), harness.assetsRoot());
    CHECK(session.writeCount() == 0U);
    CHECK_FALSE(session.lastMessage().empty());  // the reason surfaces
    CHECK(session.dirty());                      // the session copy is INTACT so the value can be fixed
    CHECK(readBytes(harness.absolutePathOf("a.aeromat")) == source);
}

TEST_CASE("material session: Revert re-reads the file and discards the session copy (ME42, seed S9)") {
    SessionHarness harness;
    const std::string source = fixtureText("canonical.aeromat");
    harness.writeAsset("a.aeromat", source);
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("a.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    const MaterialDocument original = *session.document();
    MaterialDocument edited = original;
    edited.roughnessFactor = 0.01F;
    session.edit(edited);
    REQUIRE(session.dirty());

    session.requestRevert();
    session.service(harness.db(), harness.assetsRoot());
    CHECK_FALSE(session.dirty());
    REQUIRE(session.document() != nullptr);
    CHECK((*session.document() == original));
    CHECK(session.writeCount() == 0U);  // Revert reads; it never writes
    CHECK(readBytes(harness.absolutePathOf("a.aeromat")) == source);
}

TEST_CASE("material session: an external change reloads CLEAN and notices DIRTY (ME43, AC-14, seed S10)") {
    // BOTH arms in one case on purpose: a dropped clean-reload path and a notice that fires on a
    // clean session are the same one-line mistake seen from two sides, and splitting them lets one
    // hide behind the other.
    SessionHarness harness;
    harness.writeAsset("clean.aeromat", fixtureText("canonical.aeromat"));
    harness.writeAsset("dirty.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    // --- clean: reloads SILENTLY -----------------------------------------------------------------
    engine::editor::MaterialSession clean;
    clean.reconcile("clean.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE_FALSE(clean.dirty());
    MaterialDocument fromDisk = *clean.document();
    fromDisk.name = "changed by another program";
    fromDisk.metallicFactor = 0.125F;
    harness.writeAsset("clean.aeromat", engine::writeMaterialText(fromDisk));
    harness.rescan();
    clean.reconcile("clean.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK_FALSE(clean.externalChangeNoticed());
    REQUIRE(clean.document() != nullptr);
    CHECK(clean.document()->name == "changed by another program");
    CHECK(clean.document()->metallicFactor == doctest::Approx(0.125F));
    CHECK_FALSE(clean.dirty());

    // --- dirty: KEEPS the edits and raises the notice ---------------------------------------------
    engine::editor::MaterialSession dirty;
    dirty.reconcile("dirty.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    MaterialDocument mine = *dirty.document();
    mine.roughnessFactor = 0.0625F;
    dirty.edit(mine);
    REQUIRE(dirty.dirty());
    MaterialDocument theirs = *dirty.document();
    theirs.roughnessFactor = 0.9375F;
    theirs.name = "written behind our back";
    harness.writeAsset("dirty.aeromat", engine::writeMaterialText(theirs));
    harness.rescan();
    dirty.reconcile("dirty.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(dirty.externalChangeNoticed());
    CHECK(dirty.dirty());
    REQUIRE(dirty.document() != nullptr);
    CHECK(dirty.document()->roughnessFactor == doctest::Approx(0.0625F));  // OUR edit survived
    CHECK_FALSE(dirty.lastMessage().empty());
}

TEST_CASE("material session: resetForProjectSwap clears EVERY cross-frame field (ME44, AC-15, seed S12)") {
    SessionHarness harness;
    harness.writeAsset("odd.aeromat", fixtureText("noncanonical.aeromat"));
    harness.writeAsset("bad.aeromat", fixtureText("reject-version.aeromat"));
    harness.writeAsset("plain.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    // --- a READY session with every field driven off its default ----------------------------------
    engine::editor::MaterialSession session;
    session.reconcile("odd.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE((session.state() == engine::editor::MaterialSessionState::Ready));
    MaterialDocument mine = *session.document();
    mine.roughnessFactor = 0.03125F;
    session.edit(mine);
    MaterialDocument theirs = *session.document();
    theirs.name = "external";
    harness.writeAsset("odd.aeromat", engine::writeMaterialText(theirs));
    harness.rescan();
    session.reconcile("odd.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    session.requestApply();
    session.requestRevert();

    // Every one of the twelve is now non-default. Asserted individually FIRST, so a reset that
    // silently had nothing to clear cannot pass this case.
    REQUIRE_FALSE(session.targetPath().empty());                                // targetPathValue
    REQUIRE((session.state() == engine::editor::MaterialSessionState::Ready));  // fileCopy+sessionCopy
    REQUIRE(session.document() != nullptr);
    REQUIRE(session.fileDocument() != nullptr);
    REQUIRE(session.dirty());
    REQUIRE_FALSE(session.warnings().empty());     // parseWarnings
    REQUIRE(session.externalChangeNoticed());      // externalChange
    REQUIRE_FALSE(session.lastMessage().empty());  // message
    MaterialDocument again = *session.document();
    again.metallicFactor = 0.4375F;
    session.edit(again);
    REQUIRE(session.takeDocumentChanged());  // documentChanged was set ...
    session.edit(*session.fileDocument());   // ... and is re-armed here, so the reset has one to clear
    session.edit(again);

    session.resetForProjectSwap();

    CHECK(session.targetPath().empty());
    CHECK((session.state() == engine::editor::MaterialSessionState::Untargeted));
    CHECK(session.document() == nullptr);
    CHECK(session.fileDocument() == nullptr);
    CHECK_FALSE(session.dirty());
    CHECK(session.error() == nullptr);
    CHECK(session.warnings().empty());
    CHECK_FALSE(session.externalChangeNoticed());
    CHECK(session.lastMessage().empty());
    CHECK_FALSE(session.takeDocumentChanged());

    // applyRequested / revertRequested / targetGeneration are PRIVATE, so they are asserted through
    // behaviour: target a fresh material, dirty it, and service(). A surviving Apply would write; a
    // surviving Revert would reload and clear the dirt. Neither may happen. And a surviving
    // targetGeneration would make the reconcile below take the "generation unchanged" early return
    // instead of retargeting -- which the target assertion catches.
    const std::size_t writesBefore = session.writeCount();
    session.reconcile("plain.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE(session.targetPath() == std::string_view("plain.aeromat"));
    MaterialDocument fresh = *session.document();
    fresh.occlusionStrength = 0.5F;
    session.edit(fresh);
    REQUIRE(session.dirty());
    session.service(harness.db(), harness.assetsRoot());
    CHECK(session.writeCount() == writesBefore);  // no surviving Apply
    CHECK(session.dirty());                       // no surviving Revert

    // --- an ERROR session: parseError is the one field the Ready block above cannot set ------------
    engine::editor::MaterialSession broken;
    broken.reconcile("bad.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE((broken.state() == engine::editor::MaterialSessionState::Error));
    REQUIRE(broken.error() != nullptr);
    broken.resetForProjectSwap();
    CHECK(broken.error() == nullptr);
    CHECK((broken.state() == engine::editor::MaterialSessionState::Untargeted));
}

TEST_CASE("material session: a vanished record CLEARS the session with a message (ME45, AC-14, seed S11)") {
    SessionHarness harness;
    harness.writeAsset("gone.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("gone.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE((session.state() == engine::editor::MaterialSessionState::Ready));

    harness.removeAsset("gone.aeromat");
    harness.rescan();
    session.reconcile("gone.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK((session.state() == engine::editor::MaterialSessionState::Untargeted));
    CHECK(session.targetPath().empty());
    CHECK(session.document() == nullptr);
    CHECK_FALSE(session.lastMessage().empty());
}

TEST_CASE("material session: a REJECT file is an error state, never a half-load (ME46, AC-9/AC-10)") {
    // One error, ZERO warnings -- the parser's own contract, surfaced. And the file is never
    // "repaired": it may hold a hand-recoverable value one git checkout away.
    constexpr std::array<std::string_view, 4> REJECTS{"reject-version.aeromat", "reject-token.aeromat",
                                                      "reject-nil-guid.aeromat", "reject-1e999.aeromat"};
    for (const std::string_view leaf : REJECTS) {
        CAPTURE(leaf);
        SessionHarness harness;
        const std::string source = fixtureText(leaf);
        harness.writeAsset("bad.aeromat", source);
        harness.rescan();

        engine::editor::MaterialSession session;
        session.reconcile("bad.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
        CHECK((session.state() == engine::editor::MaterialSessionState::Error));
        REQUIRE(session.error() != nullptr);
        CHECK_FALSE(session.error()->message.empty());
        CHECK(session.warnings().empty());
        CHECK(session.document() == nullptr);
        CHECK_FALSE(session.dirty());

        // Apply and Revert cannot rewrite it, and an edit cannot sneak a document in.
        MaterialDocument sneak;
        sneak.name = "not going anywhere";
        session.edit(sneak);
        CHECK(session.document() == nullptr);
        session.requestApply();
        session.service(harness.db(), harness.assetsRoot());
        CHECK(session.writeCount() == 0U);
        CHECK(readBytes(harness.absolutePathOf("bad.aeromat")) == source);
    }
}

TEST_CASE("material session: New Material's bytes are the CANONICAL default document (ME47, AC-5, seed S20)") {
    // THE TIER-0 HALF OF THE CREATE PATH. New Material writes writeMaterialText(MaterialDocument{})
    // through saveMaterialFile -- the SAME helper Apply uses, which is D12's whole point -- so what is
    // provable without a window is that those bytes are the canonical default and that they round-trip
    // to a document a session loads CLEAN. I93 is the runtime half (the drain, the name, the sidecar,
    // the selection); a seed writing a non-default document reddens both.
    SessionHarness harness;
    const std::string absolute = harness.absolutePathOf("NewMaterial.aeromat");
    REQUIRE(engine::editor::saveMaterialFile(absolute, MaterialDocument{}).empty());

    const std::string bytes = readBytes(absolute);
    CHECK(bytes == engine::writeMaterialText(MaterialDocument{}));
    // The same bytes the committed `defaulted.aeromat` fixture stands for, semantically: it is exactly
    // {"version": 1}, and both parse to a default document. This asserts the DOCUMENT, not the text,
    // because the writer is canonical and the fixture is minimal -- two legal spellings of one value.
    const engine::MaterialParseResult parsed = engine::parseMaterial(bytes);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.document.has_value());
    CHECK((*parsed.document == MaterialDocument{}));
    CHECK(parsed.warnings.empty());

    // And a session that targets it is Ready and CLEAN -- so the file the button just made is not
    // already asking to be saved (D5/INV-2: a freshly created material is not a dirty one).
    harness.rescan();
    engine::editor::MaterialSession session;
    session.reconcile("NewMaterial.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK((session.state() == engine::editor::MaterialSessionState::Ready));
    CHECK_FALSE(session.dirty());
    REQUIRE(session.document() != nullptr);
    CHECK((*session.document() == MaterialDocument{}));
}

// ---- the preview's cache key, at tier 0 (ME48) ---------------------------------------------------

TEST_CASE("material edit: one GUID in two slots is TWO cache keys, by colour space (ME48, D7, seed S22)") {
    // THE TIER-0 HALF OF D7's KEY. The preview's cache is keyed by (guid, contentHash, srgb) and its
    // own type is src-private, so what is provable here is the part that decides the interesting case:
    // the SAME source referenced by baseColor and by occlusion -- the ORM-atlas shape -- must load
    // TWICE, because a cooked artifact's colour space IS its format and those two slots want different
    // ones. Drop `srgb` from the key (seed S22) and the second slot silently samples the first slot's
    // sRGB upload as if it were linear: a plausible, wrong picture with no error anywhere.
    //
    // I91 is the runtime half and asserts the same rule as an observable count.
    const Guid shared{.hi = 0x0102030405060708ULL, .lo = 0x090A0B0C0D0E0F10ULL};
    MaterialDocument doc;
    doc.baseColor = MaterialTextureSlot{.guid = shared};
    doc.occlusion = MaterialTextureSlot{.guid = shared};

    const std::optional<MaterialTextureSlot>& base = documentSlotAt(doc, 0);
    const std::optional<MaterialTextureSlot>& occlusion = documentSlotAt(doc, 3);
    REQUIRE(base.has_value());
    REQUIRE(occlusion.has_value());
    // The GUID alone CANNOT tell them apart -- which is precisely why it is not the whole key.
    CHECK(base->guid == occlusion->guid);
    // The colour space can, and it is the slot that decides it (never the file, which has no
    // colour-space field at all -- material_format.hpp says so by name).
    CHECK(materialSlotIsSrgb(0) != materialSlotIsSrgb(3));

    // Spelled as the key comparison itself, over all five slots: two slots share a key iff they share
    // BOTH the guid and the colour space. With this document that is exactly {0,4} and {1,2,3} --
    // baseColor/emissive sample sRGB, the other three linear -- so a five-slot walk finds two distinct
    // keys for one GUID and never one.
    std::vector<std::pair<Guid, bool>> keys;
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        const std::optional<MaterialTextureSlot>& slot = documentSlotAt(doc, i);
        if (slot.has_value()) {
            const std::pair<Guid, bool> key{slot->guid, materialSlotIsSrgb(i)};
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                keys.push_back(key);
            }
        }
    }
    CHECK(keys.size() == 2U);
}

// ---- the code-review round's session closures (ME49, ME50) ---------------------------------------

TEST_CASE("material session: an UNHASHED record raises no phantom external change (ME49)") {
    // THE CODE-REVIEW ROUND'S FINDING 3. AssetRecord::contentHash is documented as MEANINGLESS unless
    // the scan actually hashed the file this pass, and an unhashed record keeps an ALL-ZERO digest --
    // which is the empty file's real value, never a sentinel. The session compared against it anyway,
    // so a scan that ran out of hash budget made a file nobody touched announce "this file changed on
    // disk; Apply will overwrite it", and it re-announced it every time the hash flipped back.
    //
    // The lever is AD55's: a starved per-scan budget, spent by the file that sorts first, leaves the
    // one that sorts second NotHashed. That REQUIRE is the case's non-vacuity -- without it a green run
    // would prove only that the budget was generous.
    SessionHarness harness;
    harness.writeAsset("a-decoy.txt", "something to spend the hash budget on");
    harness.writeAsset("z.aeromat", fixtureText("canonical.aeromat"));
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("z.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    REQUIRE((session.state() == engine::editor::MaterialSessionState::Ready));
    MaterialDocument mine = *session.document();
    mine.roughnessFactor = 0.0625F;
    session.edit(mine);
    REQUIRE(session.dirty());
    REQUIRE_FALSE(session.externalChangeNoticed());

    // A scan that hashes the decoy and runs out before reaching the material. NOTHING on disk changed.
    harness.rescanWithHashBudget(1);
    const engine::editor::AssetRecord* starved = harness.db().findByPath("z.aeromat");
    REQUIRE(starved != nullptr);
    REQUIRE((starved->change == engine::editor::ImportChange::NotHashed));
    REQUIRE_FALSE(engine::editor::assetContentHashUsable(*starved));

    session.reconcile("z.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    // NO NOTICE, and the edit is untouched: an unhashed record says nothing about the bytes, so the
    // session says nothing to the user.
    CHECK_FALSE(session.externalChangeNoticed());
    CHECK(session.dirty());
    REQUIRE(session.document() != nullptr);
    CHECK(session.document()->roughnessFactor == doctest::Approx(0.0625F));

    // AND IT DOES NOT ARM ONE FOR NEXT TIME. A scan that DOES hash the file again, still with the file
    // unchanged, must stay silent too -- which is only true if the starved scan adopted nothing. This
    // is the half that a "just skip the notice this once" fix would fail.
    harness.rescan();
    const engine::editor::AssetRecord* rehashed = harness.db().findByPath("z.aeromat");
    REQUIRE(rehashed != nullptr);
    REQUIRE(engine::editor::assetContentHashUsable(*rehashed));
    session.reconcile("z.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK_FALSE(session.externalChangeNoticed());
    CHECK(session.dirty());

    // A REAL external edit still lands, so none of the above bought its silence by going deaf.
    MaterialDocument theirs = *session.document();
    theirs.name = "written behind our back";
    harness.writeAsset("z.aeromat", engine::writeMaterialText(theirs));
    harness.rescan();
    session.reconcile("z.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK(session.externalChangeNoticed());
}

TEST_CASE("material session: an oversized .aeromat is REFUSED, never materialised (ME50)") {
    // THE CODE-REVIEW ROUND'S FINDING 11. readTextFile is documented as having NO cap and builds the
    // whole file into a std::string through an istreambuf_iterator, so one browser click on a huge file
    // that happens to be named *.aeromat was an out-of-memory abort -- inside a reconcile driven by a
    // selection change, with no way for the user to know which click did it. The preview's own texture
    // path had capped at 64 MiB since the day it shipped; this path had no cap at all.
    //
    // The fixture is written just past MAX_MATERIAL_FILE_BYTES rather than at some huge size: the cap
    // is enforced from std::filesystem::file_size ALONE, so the file is never opened and one byte over
    // is exactly as refused as a gigabyte over -- and the case costs 4 MiB of scratch instead of a
    // gigabyte of it.
    //
    // IT IS ALSO VALID JSON, padded with legal inter-token whitespace, and that is what makes the case
    // discriminate rather than merely observe. An oversized file of garbage would land in the error
    // state whether the cap fired or the parser did; this one PARSES CLEANLY the moment the cap is
    // removed, so the assertions below distinguish "refused by size" from "read and rejected".
    SessionHarness harness;
    std::string oversized = "{";
    oversized.append(static_cast<std::size_t>(engine::editor::MAX_MATERIAL_FILE_BYTES), ' ');
    oversized += "\"version\": 1}\n";
    REQUIRE(oversized.size() > engine::editor::MAX_MATERIAL_FILE_BYTES);
    harness.writeAsset("huge.aeromat", oversized);
    harness.rescan();

    engine::editor::MaterialSession session;
    session.reconcile("huge.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    // The ERROR state, with a reason -- the AC-9 posture, not a crash and not a silent Untargeted.
    CHECK(session.targetPath() == std::string_view("huge.aeromat"));
    CHECK((session.state() == engine::editor::MaterialSessionState::Error));
    REQUIRE(session.error() != nullptr);
    CHECK_FALSE(session.error()->message.empty());
    CHECK(session.error()->line == 0);  // nothing reached the JSON stage, so there is no position
    CHECK(session.document() == nullptr);
    CHECK_FALSE(session.dirty());

    // Nothing was written back, exactly as for every other unreadable file (INV-7): the bytes on disk
    // are byte-identical afterwards.
    CHECK(session.writeCount() == 0U);
    CHECK(readBytes(harness.absolutePathOf("huge.aeromat")).size() == oversized.size());

    // AND THE CAP IS NOT MERELY "BIG FILES FAIL": one byte UNDER it still loads. Without this half the
    // case would pass just as well against a cap of zero.
    const std::string canonical = fixtureText("canonical.aeromat");
    REQUIRE(canonical.size() < engine::editor::MAX_MATERIAL_FILE_BYTES);
    harness.writeAsset("fine.aeromat", canonical);
    harness.rescan();
    session.reconcile("fine.aeromat", harness.generation(), harness.db(), harness.assetsRoot());
    CHECK((session.state() == engine::editor::MaterialSessionState::Ready));
}

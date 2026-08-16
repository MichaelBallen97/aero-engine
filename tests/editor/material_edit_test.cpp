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
#include <aero/editor/material_edit.hpp>
#include <aero/editor/model_import.hpp>  // ME25-ME28: the editor value sets the four format enums mirror
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

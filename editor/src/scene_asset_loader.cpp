// Aero Engine -- the scene-asset loader (task 3.1.5, §D-9). See scene_asset_loader.hpp for the two
// rules this file exists to hold: every GPU create here runs from EditorApp::tick()'s post-draw slot
// and never from a draw walk, and nothing created here is ever owned by this class.
//
// THIS TU NEVER LOGS. Every failure is a sentence on the returned result, because the ledger owns the
// user-facing message and is the only layer that can tell Loading from Failed. The material mapping's
// own warnings are collected and returned for the same reason.
#include "scene_asset_loader.hpp"

#include <aero/assets/cooked_mesh.hpp>
#include <aero/assets/mesh_cook.hpp>
#include <aero/editor/asset_meta.hpp>  // assetContentHashUsable -- named explicitly, not via the above
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/material_edit.hpp>
#include <aero/editor/material_from_import.hpp>
#include <aero/editor/material_session.hpp>  // MAX_MATERIAL_FILE_BYTES
#include <aero/editor/mesh_cook_source.hpp>
#include <aero/editor/model_import_session.hpp>  // assignImageGuids (promoted at this task)
#include <aero/editor/project_files.hpp>         // leafOf, parentOf
#include <aero/editor/text_file.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/forward_renderer.hpp>

#include "texture_load.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace engine::editor {

namespace {

// The bytes a FileBytesResult holds, as the span every importer takes. std::string is this tree's byte
// container everywhere, so the reinterpret_cast is the same one model_import_session.cpp performs.
[[nodiscard]] std::span<const std::byte> byteSpan(const std::string& bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};  // NOLINT(*-reinterpret-cast)
}

[[nodiscard]] bool importSucceeded(ImportStatus status) noexcept {
    return status == ImportStatus::Ok || status == ImportStatus::Truncated;
}

}  // namespace

BlendArtifactResult readBlendCacheArtifact(const AssetRecord& record, std::string_view projectRootAbs) {
    BlendArtifactResult out;
    out.message = std::string(BLEND_UNCONVERTED_MESSAGE);
    // blenderExportDir is THE rule for this path and answers EMPTY for an empty project root, which is
    // the case a concatenation would turn into an absolute path at the filesystem root.
    const std::string libraryDir = blenderExportDir(projectRootAbs);
    // An unhashed record has NO CACHE KEY AT ALL (3.1.3's ThumbnailKey rule, and serviceBlend's own
    // gate): comparing against a digest this scan never computed would either hit on a lie or miss
    // forever. A nil GUID has no artifact path to build.
    if (libraryDir.empty() || !record.guid.valid() || !assetContentHashUsable(record)) {
        return out;
    }
    const std::string guidText = formatGuid(record.guid);  // returns BY VALUE -- named local first
    const std::string provenancePath = libraryDir + '/' + guidText + ".json";
    const FileReadResult provenanceText = readTextFile(provenancePath);
    if (!provenanceText.text.has_value()) {
        return out;
    }
    const std::optional<ExportProvenance> actual = parseExportProvenance(*provenanceText.text);
    if (!actual.has_value()) {
        return out;  // 3.1.2 D7's "derived data is disposable" -- a MISS, never a repair
    }
    ExportProvenance expected;
    expected.sourceHash = record.contentHash;
    expected.scriptVersion = BLENDER_SCRIPT_VERSION;
    expected.settingsFingerprint = blendExportSettingsFingerprint(record.importSettings);
    // DELIBERATELY EMPTY, and provenanceMatches' conditional arm is where the reason lives: comparing a
    // version nothing probed would require probing, and a probe is a process. This is what makes the
    // whole function spawn nothing.
    expected.blenderVersion.clear();
    if (!provenanceMatches(*actual, expected)) {
        return out;
    }
    const std::string artifactPath = libraryDir + '/' + guidText + ".glb";
    FileBytesResult artifact = readFileBytes(artifactPath, MAX_ARTIFACT_BYTES);
    if (!artifact.bytes.has_value()) {
        return out;  // a record whose artifact is missing or unusable is a MISS, exactly like a stale one
    }
    out.ok = true;
    out.message.clear();
    out.bytes = std::move(*artifact.bytes);
    out.artifactLeaf = guidText + ".glb";
    return out;
}

SceneAssetLoader::SceneAssetLoader(rhi::Device& deviceIn) noexcept : device(&deviceIn) {}

std::size_t SceneAssetLoader::importCount() const noexcept { return imports; }

std::size_t SceneAssetLoader::meshUploadCount() const noexcept { return meshUploads; }

std::size_t SceneAssetLoader::textureFailureCount() const noexcept { return textureFailures.size(); }

const SceneAssetLoader::TextureFailure* SceneAssetLoader::findFailure(const TextureFailureKey& key) const noexcept {
    for (const TextureFailure& entry : textureFailures) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

SceneAssetLoader::ModelLoadResult SceneAssetLoader::loadModel(const AssetRecord& record, std::string_view assetsRootAbs,
                                                              std::string_view projectRootAbs,
                                                              const AssetDatabase& database,
                                                              render::ForwardRenderer& renderer) {
    ModelLoadResult out;
    const std::string_view leaf = leafOf(record.relativePath);
    ImportedModel model;

    if (isBlendFileName(leaf)) {
        // THE CACHE-HIT ARM ONLY (§D-9 step 2). A miss names Import Details and stops -- no probe, no
        // spawn, no BlenderService.
        //
        // DEVIATION FROM §D-9's STEP ORDER, and the reason is the format: the plan reads the asset's
        // own bytes at step 1 and forks at step 2, but this arm never looks at a .blend's bytes at
        // all -- serviceBlend does not either. Reading them first would make a .blend above
        // MAX_MODEL_FILE_BYTES report "too large to load" even with a perfectly valid cached export
        // beside it, which is precisely the format most likely to be that large.
        const BlendArtifactResult artifact = readBlendCacheArtifact(record, projectRootAbs);
        if (!artifact.ok) {
            out.message = artifact.message;
            return out;
        }
        ++imports;
        // ONE call. Full depth. EMPTY external span. EMPTY assetRelativeDir -- the three deliberate
        // choices serviceBlend documents: the artifact lives in Library/ and has no assets-relative
        // directory, so any relative URI would resolve against an unrelated tree.
        ImportResult imported = importModel(artifact.artifactLeaf, /*assetRelativeDir=*/"", byteSpan(artifact.bytes),
                                            record.importSettings, ImportDepth::Full, {});
        if (!importSucceeded(imported.status)) {
            out.message = imported.message;
            return out;
        }
        model = std::move(imported.model);
    } else {
        // ---- the ordinary two-pass import: ModelImportSession's shape reused as a PATTERN ----------
        const std::string modelPath = std::string(assetsRootAbs) + '/' + record.relativePath;
        const FileBytesResult modelBytes = readFileBytes(modelPath, MAX_MODEL_FILE_BYTES);
        if (!modelBytes.bytes.has_value()) {
            out.message = modelBytes.refusedByCap ? "this model is too large to load (over 256 MiB)" : modelBytes.error;
            return out;
        }
        const std::string dir = parentOf(record.relativePath);
        const std::span<const std::byte> span = byteSpan(*modelBytes.bytes);

        std::vector<ExternalBuffer> externals;
        if (modelImporterNeedsExternalBuffers(leaf)) {
            ++imports;
            const ImportResult structure =
                importModel(leaf, dir, span, record.importSettings, ImportDepth::Structure, {});
            if (!importSucceeded(structure.status)) {
                out.message = structure.message;
                return out;
            }
            externals.reserve(structure.externalUris.size());
            std::uint64_t total = 0;
            bool overBudget = false;
            for (const std::string& rel : structure.externalUris) {
                const std::string path = std::string(assetsRootAbs) + '/' + rel;
                FileBytesResult buffer = readFileBytes(path, MAX_EXTERNAL_BYTES_PER_MODEL);
                if (!buffer.bytes.has_value()) {
                    continue;  // an unreadable buffer becomes MissingBuffer in pass 2, via the adapter
                }
                if (buffer.bytes->size() > MAX_EXTERNAL_BYTES_PER_MODEL - total) {
                    overBudget = true;
                    break;
                }
                total += buffer.bytes->size();
                externals.push_back(ExternalBuffer{rel, std::move(*buffer.bytes)});
            }
            if (overBudget) {
                out.message = "the external buffers exceed this importer's per-model limit";
                return out;
            }
        }
        ++imports;
        ImportResult imported = importModel(leaf, dir, span, record.importSettings, ImportDepth::Full, externals);
        if (!importSucceeded(imported.status)) {
            out.message = imported.message;
            return out;
        }
        model = std::move(imported.model);
        // `modelBytes` outlives every use above -- the named local is what guarantees it.
    }

    // §D-9 step 4: the images owe their guids to the database, and importModel itself never sees one.
    assignImageGuids(model.images, database);
    return loadFromImportedModel(model, record, renderer);
}

SceneAssetLoader::ModelLoadResult SceneAssetLoader::loadFromImportedModel(const ImportedModel& model,
                                                                          const AssetRecord& record,
                                                                          render::ForwardRenderer& renderer) {
    ModelLoadResult out;

    // ---- 5. the cook. `cooked.bytes` IS A NAMED LOCAL THAT LIVES TO THE END OF THIS FUNCTION -------
    // The retained-span contract (docs/09 §9): CookedMesh::bytes is a SPAN into exactly this vector,
    // and createMesh reads through it. Moving, clearing or scoping this local is seed S22, whose only
    // oracle is ASan -- nothing between here and the createMesh below may touch it.
    const assets::MeshCookResult cooked = cookImportedModel(model, record.guid);
    if (cooked.bytes.empty()) {
        out.message = "this model could not be cooked: " + cooked.message;
        return out;
    }

    // ---- 6. the parse -----------------------------------------------------------------------------
    const assets::CookedMeshParseResult parsed = assets::parseCookedMesh(cooked.bytes);
    if (parsed.status != assets::CookedMeshStatus::Ok) {
        out.message = std::format("this cooked mesh could not be read ({}): {}",
                                  assets::cookedMeshStatusLabel(parsed.status), parsed.message);
        return out;
    }

    // ---- 7. the upload. THE ONE CALL WHOSE ARGUMENT BORROWS `cooked.bytes` -------------------------
    ++meshUploads;
    const render::MeshHandle mesh = renderer.createMesh(parsed.mesh);
    if (!mesh.valid()) {
        out.message = "the GPU refused this mesh (see the Console for the reason)";
        return out;
    }
    out.handles.mesh = mesh;

    // ---- 8. one MaterialHandle per ImportedMaterial, in SOURCE ORDER ------------------------------
    out.handles.materials.reserve(model.materials.size());
    out.handles.materialStates.reserve(model.materials.size());
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        MaterialFromImportResult document = materialDocumentFromImported(model.materials[i], model.images);
        for (std::string& warning : document.warnings) {
            out.warnings.push_back(std::move(warning));
        }
        const render::MaterialParams params = materialParamsFor(document.document);
        render::MaterialTextureSlots slots{};
        // DECLARATION ORDER IS BINDING ORDER (material.hpp's contract) -- material_preview.cpp's own
        // idiom, so this array and documentSlotAt index the same five things in the same order.
        const std::array<render::MaterialTextureSlot*, render::MATERIAL_TEXTURE_SLOT_COUNT> bound{
            &slots.baseColor, &slots.metallicRoughness, &slots.normal, &slots.occlusion, &slots.emissive};
        for (std::size_t slot = 0; slot < render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
            const std::optional<MaterialTextureSlot>& documentSlot = documentSlotAt(document.document, slot);
            if (!documentSlot.has_value()) {
                continue;  // unbound: the built-in default, with the desc's own defaults
            }
            bound[slot]->sampler = materialSamplerDescFor(*documentSlot);
            // EVERY SLOT TEXTURE STAYS INVALID HERE -- default texels showing (the 3.4.1 doctrine). The
            // request below is what dresses it, one per service pass.
            if (!documentSlot->guid.valid()) {
                continue;  // §11 forbids a present-but-nil slot; defence in depth, never a live path
            }
            out.textureRequests.push_back(TextureRequest{
                .materialIndex = i, .slot = slot, .guid = documentSlot->guid, .srgb = materialSlotIsSrgb(slot)});
        }
        out.handles.materials.push_back(renderer.createMaterial(params, slots));
        out.handles.materialStates.push_back(MaterialRuntimeState{.params = params, .slots = slots});
    }

    // ---- 9. the binding, in cooked submesh order --------------------------------------------------
    out.binding.mesh = mesh;
    out.binding.submeshes.reserve(parsed.mesh.submeshes.size());
    for (std::size_t p = 0; p < parsed.mesh.submeshes.size(); ++p) {
        const assets::CookedSubmesh& submesh = parsed.mesh.submeshes[p];
        // COOKED_INVALID_MATERIAL yields an INVALID handle, which is correct and is the
        // renderer-default path -- not an error, and never a log.
        const bool resolvable = submesh.materialIndex < out.handles.materials.size();
        out.binding.submeshes.push_back(scene_render::MeshBindingSubmesh{
            .submesh = static_cast<std::uint32_t>(p),
            .sourceMeshIndex = submesh.sourceMeshIndex,
            .material = resolvable ? out.handles.materials[submesh.materialIndex] : render::MaterialHandle{}});
    }

    // ---- 10. bounds, folded per sourceMeshIndex ---------------------------------------------------
    // §9.5/§9.8 make CookedSubmesh::bounds node-independent -- folded from `positions` alone -- which
    // is exactly what an entity-LOCAL box must be, and is why no node transform enters this.
    for (const assets::CookedSubmesh& submesh : parsed.mesh.submeshes) {
        const auto it = std::find_if(out.handles.bounds.begin(), out.handles.bounds.end(),
                                     [&submesh](const std::pair<std::uint32_t, Aabb>& entry) noexcept {
                                         return entry.first == submesh.sourceMeshIndex;
                                     });
        Aabb& box = it != out.handles.bounds.end()
                        ? it->second
                        : out.handles.bounds.emplace_back(submesh.sourceMeshIndex, Aabb::empty()).second;
        box.expand(submesh.bounds.min);
        box.expand(submesh.bounds.max);
    }

    out.ok = true;
    return out;
    // The ImportedModel is the caller's and is dropped there: importer memory is the peak (3.3.1's R7
    // measured ~3.1x source for a binary .gltf and ~9.0x for an ASCII .obj) and it is transient by
    // design.
}

SceneAssetLoader::MaterialLoadResult SceneAssetLoader::loadMaterial(const AssetRecord& record,
                                                                    std::string_view assetsRootAbs,
                                                                    render::ForwardRenderer& renderer) {
    MaterialLoadResult out;
    const std::string materialPath = std::string(assetsRootAbs) + '/' + record.relativePath;
    const FileBytesResult bytes = readFileBytes(materialPath, MAX_MATERIAL_FILE_BYTES);
    if (!bytes.bytes.has_value()) {
        out.message = bytes.refusedByCap ? "this material is too large to load (over 4 MiB)" : bytes.error;
        return out;
    }
    MaterialParseResult parsed = parseMaterial(*bytes.bytes);
    // Spelled as has_value() rather than as ok(), which IS this same test (material_format.cpp:556):
    // bugprone-unchecked-optional-access does not reason across a member function and is an error on
    // the Linux Debug lane.
    if (!parsed.document.has_value()) {
        out.message = parsed.error.message;  // the parse error's OWN sentence, never a second wording
        return out;
    }
    const MaterialDocument& document = *parsed.document;
    // An unknown key is the one thing a rewrite DELETES, so the parser's own findings travel up rather
    // than being re-derived or dropped.
    for (std::string& warning : parsed.warnings) {
        out.warnings.push_back(std::move(warning));
    }

    const render::MaterialParams params = materialParamsFor(document);
    render::MaterialTextureSlots slots{};
    const std::array<render::MaterialTextureSlot*, render::MATERIAL_TEXTURE_SLOT_COUNT> bound{
        &slots.baseColor, &slots.metallicRoughness, &slots.normal, &slots.occlusion, &slots.emissive};
    for (std::size_t slot = 0; slot < render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
        const std::optional<MaterialTextureSlot>& documentSlot = documentSlotAt(document, slot);
        if (!documentSlot.has_value()) {
            continue;
        }
        bound[slot]->sampler = materialSamplerDescFor(*documentSlot);
        if (!documentSlot->guid.valid()) {
            continue;
        }
        out.textureRequests.push_back(TextureRequest{
            .materialIndex = 0, .slot = slot, .guid = documentSlot->guid, .srgb = materialSlotIsSrgb(slot)});
    }
    out.material = renderer.createMaterial(params, slots);
    out.state = MaterialRuntimeState{.params = params, .slots = slots};
    out.ok = out.material.valid();
    if (!out.ok) {
        out.message = "the GPU refused this material (see the Console for the reason)";
    }
    return out;
}

SceneAssetLoader::TextureLoadResult SceneAssetLoader::loadSlotTexture(const AssetRecord& record,
                                                                      std::string_view assetsRootAbs, bool srgb) {
    TextureLoadResult out;
    if (device == nullptr) {
        out.message = "This texture cannot be loaded -- no GPU device.";
        return out;
    }
    const bool keyed = assetContentHashUsable(record);
    const TextureFailureKey key{.guid = record.guid, .hash = record.contentHash, .srgb = srgb};
    if (keyed) {
        if (const TextureFailure* known = findFailure(key); known != nullptr) {
            out.message = known->message;  // STICKY: one decode per key per session, never one per pass
            return out;
        }
    }
    const std::string absolutePath = std::string(assetsRootAbs) + '/' + record.relativePath;
    const LoadedTexture loaded = loadTextureFromSourceFile(*device, absolutePath, srgb);
    if (!loaded.error.empty()) {
        if (keyed) {
            textureFailures.push_back(TextureFailure{.key = key, .message = loaded.error});
        }
        out.message = loaded.error;
        return out;
    }
    out.ok = true;
    out.texture = loaded.texture;
    return out;
}

void SceneAssetLoader::rebindSlot(render::ForwardRenderer& renderer, render::MaterialHandle material,
                                  const render::MaterialParams& params, render::MaterialTextureSlots& slots,
                                  std::size_t slot, rhi::TextureHandle texture) {
    if (!material.valid() || !texture.valid() || slot >= render::MATERIAL_TEXTURE_SLOT_COUNT) {
        return;
    }
    const std::array<render::MaterialTextureSlot*, render::MATERIAL_TEXTURE_SLOT_COUNT> bound{
        &slots.baseColor, &slots.metallicRoughness, &slots.normal, &slots.occlusion, &slots.emissive};
    bound[slot]->texture = texture;
    (void)renderer.updateMaterial(material, params, slots);
}

}  // namespace engine::editor

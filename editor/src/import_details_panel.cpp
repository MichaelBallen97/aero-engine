// Aero Engine — the Import Details panel's ONE ImGui TU (task 3.2.1). Draws the on-demand model
// import session's result; RECORDS Apply/Revert/settings-edit requests and WRITES NOTHING (D17/
// INV-M12) -- EditorApp::tick() is the only place a request becomes a file write or a session
// mutation, exactly the "record a pending action, apply it after the walk" rule every panel in this
// tree already follows.
//
// Every walk here is ITERATIVE, with an explicit stack for the node hierarchy (misc-no-recursion is
// --warnings-as-errors on the Linux lane) -- meshes/materials/skins/animations are flat loops, never a
// tree, so they need no stack at all.
//
// A dynamic string is NEVER a format argument (project_settings_panel.cpp's own rule, applied here a
// second time): every draw call goes through a named local built with std::format, then passed as a
// "%s" argument.
//
// ASCII ONLY in every literal (3.1.3's post-merge lesson): the editor loads no font of its own, and
// ImGui's ProggyClean covers Basic + Extended Latin only -- "." / "-" / "..." stand in for full-width
// punctuation.
#include "import_details_panel.hpp"

#include <aero/editor/blender_service.hpp>  // task 3.2.4: the Blender section reads it, never writes it
#include <aero/editor/model_import.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>  // task 3.2.2 (§A-2): leafOf, for the importer-identity fix

#include <cstddef>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <optional>
#include <string>
#include <unordered_map>  // task 3.2.2 (R13 fix): localId -> nodes[] position, see drawHierarchy
#include <vector>

namespace engine::editor {

namespace {

constexpr ImVec4 WARNING_COLOR{1.0F, 0.4F, 0.4F, 1.0F};  // project_ui.cpp's own error-text colour

[[nodiscard]] const char* alphaModeLabel(AlphaMode mode) noexcept {
    switch (mode) {
        case AlphaMode::Opaque:
            return "Opaque";
        case AlphaMode::Mask:
            return "Mask";
        case AlphaMode::Blend:
            return "Blend";
    }
    return "?";  // unreachable -- every enumerator handled above
}

[[nodiscard]] const char* interpolationLabel(AnimationInterpolation mode) noexcept {
    switch (mode) {
        case AnimationInterpolation::Linear:
            return "Linear";
        case AnimationInterpolation::Step:
            return "Step";
        case AnimationInterpolation::CubicSpline:
            return "CubicSpline";
    }
    return "?";  // unreachable -- every enumerator handled above
}

// "POS NRM TAN UV0 UV1 COL JNT WGT", one badge per bit actually present -- what section 4 (Meshes)
// shows beside each primitive's vertex/triangle counts.
[[nodiscard]] std::string attributeBadges(VertexAttribute attributes) {
    std::string badges;
    const auto add = [&badges](bool present, const char* tag) {
        if (present) {
            if (!badges.empty()) {
                badges += ' ';
            }
            badges += tag;
        }
    };
    add(has(attributes, VertexAttribute::Position), "POS");
    add(has(attributes, VertexAttribute::Normal), "NRM");
    add(has(attributes, VertexAttribute::Tangent), "TAN");
    add(has(attributes, VertexAttribute::TexCoord0), "UV0");
    add(has(attributes, VertexAttribute::TexCoord1), "UV1");
    add(has(attributes, VertexAttribute::Color0), "COL");
    add(has(attributes, VertexAttribute::Joints0), "JNT");
    add(has(attributes, VertexAttribute::Weights0), "WGT");
    return badges.empty() ? std::string("(none)") : badges;
}

// The "... and N more" tail every capped list in this tree uses (logCappedWarn's shape, applied here).
void drawWarnings(const ImportResult& result) {
    if (result.warnings.empty() && result.warningTotal == 0) {
        return;
    }
    ImGui::SeparatorText("Warnings");
    for (const std::string& warning : result.warnings) {
        ImGui::TextWrapped("%s", warning.c_str());
    }
    if (result.warningTotal > result.warnings.size()) {
        const std::string more = std::format("... and {} more", result.warningTotal - result.warnings.size());
        ImGui::TextDisabled("%s", more.c_str());
    }
}

// ---- section 1: Overview ---------------------------------------------------------------------------
void drawOverview(const ModelImportSession& session) {
    const ImportResult& result = session.result();
    const ImportSummary& summary = result.model.summary;

    // A2: the importer identity is a PURE FUNCTION OF THE FILE NAME (model_import.hpp) -- never a
    // constant. Before this, an .fbx said "Importer: gltf / 1", which validation row 1 reads first.
    const ImporterIdentity identity = modelImporterIdentity(leafOf(session.target()));
    const std::string importerLine = identity.name.empty()
                                         ? std::string("Importer: --")
                                         : std::format("Importer: {} / {}", identity.name, identity.version);
    ImGui::TextUnformatted(importerLine.c_str());
    const std::string statusLine = std::format("Status: {}", importStatusLabel(result.status));
    ImGui::TextUnformatted(statusLine.c_str());
    const std::string sizeLine = std::format("File size: {} bytes", session.fileSizeBytes());
    ImGui::TextUnformatted(sizeLine.c_str());

    const std::string counts1 = std::format("Nodes: {}  Meshes: {}  Primitives: {}", summary.nodeCount,
                                            summary.meshCount, summary.primitiveCount);
    ImGui::TextUnformatted(counts1.c_str());
    const std::string counts2 =
        std::format("Materials: {}  Images: {}  Skins: {}  Joints: {}  Animations: {}", summary.materialCount,
                    summary.imageCount, summary.skinCount, summary.jointCount, summary.animationCount);
    ImGui::TextWrapped("%s", counts2.c_str());
    const std::string counts3 = std::format("Vertices: {}  Triangles: {}", summary.vertexCount, summary.triangleCount);
    ImGui::TextUnformatted(counts3.c_str());

    // D19: ONE row, drawn only when the source format declares a space of its own. glTF declares none
    // (metres / Y-up / right-handed BY SPECIFICATION), so this is absent for every .gltf -- which is
    // AC-60, and is why the row means something: a .fbx and a .gltf reporting identical bounds is only
    // meaningful if you can SEE that the FBX started somewhere else.
    // The arrow's right-hand side is a CONSTANT -- this importer always targets Y-up, 1 m -- so the row
    // reads as a statement about the conversion, not a pair of unrelated facts.
    if (result.model.sourceSpace.declared) {
        const SourceSpace& sp = result.model.sourceSpace;
        std::string provenance;         // A21: a hand-written FBX has NO
        if (!sp.generator.empty()) {    // exporter, so the parenthetical
            provenance = sp.generator;  // must not render as an empty "()"
        }
        if (!sp.formatVersion.empty()) {
            provenance += provenance.empty() ? sp.formatVersion : ", " + sp.formatVersion;
        }
        const std::string spaceLine =
            std::format("Source space: {}-up, {:g} m/unit  ->  Y-up, 1 m/unit{}", sp.upAxis, sp.unitMeters,
                        provenance.empty() ? std::string() : std::format("   ({})", provenance));
        ImGui::TextWrapped("%s", spaceLine.c_str());
    }

    // A21: the fold's own bounds, NEVER the document's accessor min/max -- and at Structure depth (no
    // positions decoded) `bounds` is the empty, INVALID Aabb, so this reads "--" rather than a fake box.
    if (summary.bounds.valid()) {
        const Vec3 boundsSize = summary.bounds.size();
        const std::string minLine = std::format("Bounds min:  {:.4f}, {:.4f}, {:.4f}", summary.bounds.min.x,
                                                summary.bounds.min.y, summary.bounds.min.z);
        const std::string maxLine = std::format("Bounds max:  {:.4f}, {:.4f}, {:.4f}", summary.bounds.max.x,
                                                summary.bounds.max.y, summary.bounds.max.z);
        const std::string sizeLine2 =
            std::format("Bounds size: {:.4f}, {:.4f}, {:.4f}", boundsSize.x, boundsSize.y, boundsSize.z);
        ImGui::TextUnformatted(minLine.c_str());
        ImGui::TextUnformatted(maxLine.c_str());
        ImGui::TextUnformatted(sizeLine2.c_str());
    } else {
        ImGui::TextDisabled("Bounds: --");
    }

    if (!result.message.empty()) {  // a Truncated Ok still carries a message naming the cap it hit
        ImGui::TextWrapped("%s", result.message.c_str());
    }
    drawWarnings(result);
}

// ---- section 2: Import Settings ----------------------------------------------------------------------
// `applyRequested`/`revertRequested`/`editedSettings` are the PANEL's own one-shot members, passed by
// reference from onDraw() -- this function RECORDS a request; it never calls into `session` to change
// anything (INV-M12/D17: the write happens in EditorApp::tick(), never inside onDraw()).
void drawSettingsForm(const ModelImportSession& session, bool& applyRequested, bool& revertRequested,
                      std::optional<ImportSettings>& editedSettings) {
    // Rebuilt from the session's OWN pending copy every frame (never a member) -- the session is the
    // single source of truth, and EditorApp::tick() drains `editedSettings` into it exactly one tick
    // after this function records an edit, so the value read back here always matches what was last
    // recorded (E12/E13: a hand-edited .meta's zero/negative scale is HONOURED, never silently clamped
    // by this widget -- v_min only bounds what DRAGGING can produce).
    ImportSettings form = session.pendingSettings();
    bool changed = false;
    changed = ImGui::DragFloat("Scale", &form.scale, 0.01F, 0.0001F, 1000.0F, "%.4f") || changed;
    changed = ImGui::Checkbox("Import materials", &form.importMaterials) || changed;
    changed = ImGui::Checkbox("Import animations", &form.importAnimations) || changed;
    changed = ImGui::Checkbox("Import skins", &form.importSkins) || changed;
    if (changed) {
        editedSettings = form;
    }

    const bool canApply = session.canApply();
    ImGui::BeginDisabled(!canApply);  // 1:1 with EndDisabled; no continue/return between (project_ui.cpp's rule)
    if (ImGui::Button("Apply")) {
        applyRequested = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        revertRequested = true;
    }
    if (!session.applyError().empty()) {
        ImGui::TextColored(WARNING_COLOR, "%s", session.applyError().c_str());
    }
}

// ---- section 3: Hierarchy ----------------------------------------------------------------------------
// One explicit-stack frame per node ITERATIVELY VISITED (misc-no-recursion): `childCursor` is the next
// child index still to push, and `opened` echoes what TreeNodeEx returned -- TreePop is owed iff true,
// exactly TreeNodeEx's own contract (a leaf node still reports `open == true`, imgui.h:1360).
//
// task 3.2.2 BUGFIX (R13, found by this task's own I61/I63 GPU-tier cases -- an ASan heap-buffer-
// overflow, not merely a wrong picture): `ImportedNode::localId` is the source's OWN identifier, and
// `ImportedModel::roots` / `ImportedNode::children` hold localIds, NEVER positions in
// `ImportedModel::nodes` (model_import.hpp's own documented contract). For glTF the two happen to
// coincide, which is why this walked `model.nodes[id]` directly since task 3.2.1 and never crashed --
// for FBX, `localId` is the raw ufbx `typed_id` and is offset from the vector position the instant the
// scene root (skipped, A13) is involved, so `model.nodes[id]` reads out of bounds for any real FBX
// hierarchy. Frames now hold a `nodeLocalId`, translated to a `nodes[]` position through the SAME
// localId -> position map `fbx_import.cpp`'s own `nodeIndexByLocalId` already builds on the import
// side (`.claude/rules/editor.md`'s R13 entry: "Read this before indexing nodes[localId]").
struct HierarchyFrame {
    std::uint32_t nodeLocalId = 0;
    std::size_t childCursor = 0;
    bool entered = false;
    bool opened = false;
};

void drawNodeTree(const ImportedModel& model, const std::unordered_map<std::uint32_t, std::uint32_t>& indexByLocalId,
                  std::uint32_t rootLocalId, std::vector<HierarchyFrame>& stack) {
    stack.clear();
    stack.push_back(HierarchyFrame{.nodeLocalId = rootLocalId});
    while (!stack.empty()) {
        // Re-fetched every iteration, NEVER held across a push_back (which can reallocate `stack`).
        HierarchyFrame& top = stack.back();
        const auto found = indexByLocalId.find(top.nodeLocalId);
        if (found == indexByLocalId.end()) {
            // Defensive (E17's own posture): an out-of-range localId from a malformed source. Never
            // entered, so nothing was pushed onto ImGui's own ID/tree stacks for it -- pop and move on.
            stack.pop_back();
            continue;
        }
        const std::uint32_t nodeIndex = found->second;
        if (!top.entered) {
            top.entered = true;
            const ImportedNode& node = model.nodes[nodeIndex];
            ImGui::PushID(static_cast<int>(nodeIndex));
            std::string label = node.name.empty() ? std::format("<node {}>", nodeIndex) : node.name;
            if (node.meshIndex != INVALID_SUBASSET) {
                label += " [mesh]";
            }
            if (node.skinIndex != INVALID_SUBASSET) {
                label += " [skin]";
            }
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
            if (node.children.empty()) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            top.opened = ImGui::TreeNodeEx("##row", flags, "%s", label.c_str());
        }
        const ImportedNode& current = model.nodes[nodeIndex];
        if (top.opened && top.childCursor < current.children.size()) {
            const std::uint32_t childLocalId = current.children[top.childCursor];
            ++top.childCursor;
            stack.push_back(HierarchyFrame{.nodeLocalId = childLocalId});
            continue;  // `top` is not touched again this iteration
        }
        if (top.opened) {
            ImGui::TreePop();
        }
        ImGui::PopID();
        stack.pop_back();
    }
}

void drawHierarchy(const ImportedModel& model) {
    if (model.roots.empty()) {
        ImGui::TextDisabled("(no nodes)");
        return;
    }
    // Built ONCE per call -- fbx_import.cpp's own nodeIndexByLocalId shape, restated on the READ side.
    std::unordered_map<std::uint32_t, std::uint32_t> indexByLocalId;
    indexByLocalId.reserve(model.nodes.size());
    for (std::uint32_t i = 0; i < model.nodes.size(); ++i) {
        indexByLocalId.emplace(model.nodes[i].localId, i);
    }
    std::vector<HierarchyFrame> stack;  // local: this panel shows one model at a time, not a hot loop
    for (const std::uint32_t root : model.roots) {
        drawNodeTree(model, indexByLocalId, root, stack);
    }
}

// ---- section 4: Meshes -------------------------------------------------------------------------------
void drawMeshes(const ImportedModel& model) {
    if (model.meshes.empty()) {
        ImGui::TextDisabled("(no meshes)");
        return;
    }
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const ImportedMesh& mesh = model.meshes[m];
        ImGui::PushID(static_cast<int>(m));
        const std::string meshName = mesh.name.empty() ? std::format("<mesh {}>", m) : mesh.name;
        const std::string header = std::format("{} ({} primitive(s))", meshName, mesh.primitives.size());
        constexpr ImGuiTreeNodeFlags MESH_FLAGS = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
        if (ImGui::TreeNodeEx("##mesh", MESH_FLAGS, "%s", header.c_str())) {
            for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
                const ImportedPrimitive& primitive = mesh.primitives[p];
                std::string materialName = "(none)";
                if (primitive.materialIndex != INVALID_SUBASSET && primitive.materialIndex < model.materials.size()) {
                    const ImportedMaterial& material = model.materials[primitive.materialIndex];
                    materialName =
                        material.name.empty() ? std::format("<material {}>", primitive.materialIndex) : material.name;
                }
                const std::string line = std::format("Primitive {}: {} vertices, {} triangles, [{}], material: {}", p,
                                                     primitive.positions.size(), primitive.indices.size() / 3,
                                                     attributeBadges(primitive.attributes), materialName);
                ImGui::TextWrapped("%s", line.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---- section 5: Materials ----------------------------------------------------------------------------
// A10: a slot resolves through TWO hops (TextureInfo -> Texture -> image), and an absent slot is
// std::nullopt, never index 0 -- shown here as "(none)", never a fake path.
void drawTextureSlot(const ImportedModel& model, const char* slotName, const std::optional<ImportedTextureRef>& slot) {
    if (!slot.has_value()) {
        const std::string line = std::format("{}: (none)", slotName);
        ImGui::TextDisabled("%s", line.c_str());
        return;
    }
    if (slot->imageIndex >= model.images.size()) {
        const std::string line = std::format("{}: (unresolved)", slotName);
        ImGui::TextDisabled("%s", line.c_str());
        return;
    }
    const ImportedImage& image = model.images[slot->imageIndex];
    if (!image.refusal.empty()) {  // D14: what makes a broken texture reference visible, not mysterious
        const std::string line = std::format("{}: {}", slotName, image.refusal);
        ImGui::TextColored(WARNING_COLOR, "%s", line.c_str());
        return;
    }
    if (!image.relativePath.empty()) {
        const std::string line = std::format("{}: {}", slotName, image.relativePath);
        ImGui::TextWrapped("%s", line.c_str());
        return;
    }
    const std::string line = std::format("{}: (embedded)", slotName);  // a data: URI or a GLB bufferView
    ImGui::TextWrapped("%s", line.c_str());
}

void drawMaterials(const ImportedModel& model) {
    if (model.materials.empty()) {
        ImGui::TextDisabled("(no materials)");
        return;
    }
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        const ImportedMaterial& material = model.materials[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string materialName = material.name.empty() ? std::format("<material {}>", i) : material.name;
        constexpr ImGuiTreeNodeFlags MATERIAL_FLAGS = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
        if (ImGui::TreeNodeEx("##material", MATERIAL_FLAGS, "%s", materialName.c_str())) {
            const std::string factors =
                std::format("Base colour: {:.3f}, {:.3f}, {:.3f}, {:.3f}   Metallic: {:.3f}   Roughness: {:.3f}",
                            material.baseColorFactor.x, material.baseColorFactor.y, material.baseColorFactor.z,
                            material.baseColorFactor.w, material.metallicFactor, material.roughnessFactor);
            ImGui::TextWrapped("%s", factors.c_str());
            const std::string more =
                std::format("Emissive: {:.3f}, {:.3f}, {:.3f}   Normal scale: {:.3f}   Occlusion: {:.3f}",
                            material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z,
                            material.normalScale, material.occlusionStrength);
            ImGui::TextWrapped("%s", more.c_str());
            const std::string alpha =
                std::format("Alpha: {} (cutoff {:.3f})   Double-sided: {}", alphaModeLabel(material.alphaMode),
                            material.alphaCutoff, material.doubleSided ? "yes" : "no");
            ImGui::TextWrapped("%s", alpha.c_str());
            drawTextureSlot(model, "Base colour", material.baseColor);
            drawTextureSlot(model, "Metallic/roughness", material.metallicRoughness);
            drawTextureSlot(model, "Normal", material.normal);
            drawTextureSlot(model, "Occlusion", material.occlusion);
            drawTextureSlot(model, "Emissive", material.emissive);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---- section 6: Skeleton & Animation -------------------------------------------------------------------
// task 3.2.2 BUGFIX (R13, the identical defect drawHierarchy above had): `ImportedSkin::joints` holds
// NODE localIds too (fbx_import.cpp's own phase 7 pushes `cluster->bone_node->typed_id` verbatim), so
// `model.nodes[jointNode]` is the SAME out-of-bounds read for any FBX skin, and `jointNode <
// model.nodes.size()` is the wrong guard entirely (a localId can be numerically smaller than
// nodes.size() and still name the WRONG node).
void drawSkeletonAndAnimation(const ImportedModel& model) {
    if (model.skins.empty() && model.animations.empty()) {
        ImGui::TextDisabled("(no skins or animations)");
        return;
    }
    std::unordered_map<std::uint32_t, std::uint32_t> indexByLocalId;
    indexByLocalId.reserve(model.nodes.size());
    for (std::uint32_t i = 0; i < model.nodes.size(); ++i) {
        indexByLocalId.emplace(model.nodes[i].localId, i);
    }
    constexpr ImGuiTreeNodeFlags SUB_FLAGS = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    for (std::size_t s = 0; s < model.skins.size(); ++s) {
        const ImportedSkin& skin = model.skins[s];
        ImGui::PushID(static_cast<int>(s));
        const std::string skinName = skin.name.empty() ? std::format("<skin {}>", s) : skin.name;
        const std::string header = std::format("{} ({} joint(s))", skinName, skin.joints.size());
        if (ImGui::TreeNodeEx("##skin", SUB_FLAGS, "%s", header.c_str())) {
            for (const std::uint32_t jointLocalId : skin.joints) {
                std::string jointName = "<?>";
                if (const auto found = indexByLocalId.find(jointLocalId); found != indexByLocalId.end()) {
                    const ImportedNode& node = model.nodes[found->second];
                    jointName = node.name.empty() ? std::format("<node {}>", found->second) : node.name;
                }
                ImGui::TextWrapped("%s", jointName.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    for (std::size_t a = 0; a < model.animations.size(); ++a) {
        const ImportedAnimation& animation = model.animations[a];
        ImGui::PushID(static_cast<int>(a));
        const std::string clipName = animation.name.empty() ? std::format("<animation {}>", a) : animation.name;
        const std::string header =
            std::format("{} ({:.3f}s, {} channel(s))", clipName, animation.duration, animation.channels.size());
        if (ImGui::TreeNodeEx("##clip", SUB_FLAGS, "%s", header.c_str())) {
            bool sawLinear = false;
            bool sawStep = false;
            bool sawCubic = false;
            for (const ImportedAnimationChannel& channel : animation.channels) {
                switch (channel.interpolation) {
                    case AnimationInterpolation::Linear:
                        sawLinear = true;
                        break;
                    case AnimationInterpolation::Step:
                        sawStep = true;
                        break;
                    case AnimationInterpolation::CubicSpline:
                        sawCubic = true;
                        break;
                }
            }
            std::string modes;
            const auto addMode = [&modes](bool present, const char* name) {
                if (present) {
                    if (!modes.empty()) {
                        modes += ", ";
                    }
                    modes += name;
                }
            };
            addMode(sawLinear, interpolationLabel(AnimationInterpolation::Linear));
            addMode(sawStep, interpolationLabel(AnimationInterpolation::Step));
            addMode(sawCubic, interpolationLabel(AnimationInterpolation::CubicSpline));
            const std::string modesLine =
                std::format("Interpolation: {}", modes.empty() ? std::string("(none)") : modes);
            ImGui::TextWrapped("%s", modesLine.c_str());
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---- the Imported state's six CollapsingHeader sections, in order -----------------------------------
// ALL SIX default OPEN (the Inspector's own per-component CollapsingHeader precedent,
// inspector_panel.cpp:186, applied uniformly): a user who just selected an asset sees everything about
// it immediately, with no extra click to discover content -- and, structurally, a section that
// defaulted CLOSED would be unreachable by this project's entire GPU-tier testing methodology (no
// tier anywhere in this tree can synthesize a CollapsingHeader click), leaving it permanently unproven.
void drawImported(const ModelImportSession& session, bool& applyRequested, bool& revertRequested,
                  std::optional<ImportSettings>& editedSettings) {
    if (ImGui::CollapsingHeader("Overview", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawOverview(session);
    }
    if (ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawSettingsForm(session, applyRequested, revertRequested, editedSettings);
    }
    if (ImGui::CollapsingHeader("Hierarchy", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawHierarchy(session.result().model);
    }
    if (ImGui::CollapsingHeader("Meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawMeshes(session.result().model);
    }
    if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawMaterials(session.result().model);
    }
    if (ImGui::CollapsingHeader("Skeleton & Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawSkeletonAndAnimation(session.result().model);
    }
}

// ---- task 3.2.4: the Blender section -----------------------------------------------------------------
//
// Drawn BEFORE the state switch and gated on isBlendFileName(target) ALONE, so it renders identically
// in NeedsConversion, Converting, ConversionFailed and Imported (AC-37). Every arm of that switch
// returns, so a section placed inside one would render in that state only -- and `Re-import` on a cache
// hit, `Cancel` mid-run and the disabled button on a nil GUID each live in a different state.
//
// IT CALLS NO MUTATING MEMBER OF ANYTHING (AC-39) and performs no file access: `logTail()` is already
// loaded by poll() when a run ends, so the panel renders what it holds and needs no fifth channel.
void drawBlenderLog(const BlenderService& blender, std::string& scratch) {
    if (blender.logPath().empty()) {
        return;
    }
    // DEFAULT-OPEN, for this file's own stated reason (code-review NOTE 11): "a section that defaulted
    // CLOSED would be unreachable by this project's entire GPU-tier testing methodology", because no
    // tier here can synthesize a click. This node shipped closed, so everything inside it -- including
    // the refused-by-cap branch, which is the one that formats a byte count and an absolute path --
    // never executed in any test, on any lane, under any sanitizer. It is also the better default: a
    // failure the user is reading about is exactly when the log matters.
    if (ImGui::TreeNodeEx("Blender log", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (blender.logRefusedByCap()) {
            // NEVER a partial read presented as the whole log (AC-35): the byte count and the absolute
            // path, so the user can open it themselves.
            scratch = std::format("The log is {} bytes, above the {}-byte display limit. Open it directly: {}",
                                  blender.logSizeBytes(), MAX_TOOL_LOG_BYTES, blender.logPath());
            ImGui::TextWrapped("%s", scratch.c_str());
        } else if (blender.logTail().empty()) {
            scratch = std::format("The log is empty. It is at: {}", blender.logPath());
            ImGui::TextWrapped("%s", scratch.c_str());
        } else {
            ImGui::TextWrapped("%s", blender.logTail().c_str());
        }
        ImGui::TreePop();
    }
}

void drawSearchedPaths(const BlenderService& blender) {
    if (blender.searchedPaths().empty()) {
        return;
    }
    ImGui::TextDisabled("Searched:");
    // A scrolling region rather than an unbounded list: a long PATH can contribute hundreds of entries.
    if (ImGui::BeginChild("blender-searched", ImVec2(0.0F, ImGui::GetTextLineHeightWithSpacing() * 6.0F),
                          ImGuiChildFlags_Borders)) {
        for (const std::string& path : blender.searchedPaths()) {
            ImGui::TextUnformatted(path.c_str());
        }
    }
    ImGui::EndChild();  // 1:1 with BeginChild regardless of what it returned
}

void drawBlenderSection(const ModelImportSession& session, bool& convertRequested, bool& cancelRequested,
                        bool& locateRequested, bool& redetectRequested, std::string& scratch) {
    const BlenderService& blender = session.blender();

    if (!session.targetHasIdentity()) {
        // AC-27. NeedsConversion, never NotImportable -- and the button is DRAWN and DISABLED rather
        // than absent, so the reason is visible instead of the control silently vanishing.
        ImGui::TextWrapped(
            "This file has no valid .meta identity, so there is nowhere to cache a conversion. "
            "Reimport the project's assets to repair it.");
        ImGui::BeginDisabled(true);
        if (ImGui::Button("Import with Blender")) {
            convertRequested = true;  // unreachable while disabled; kept so enabling it is one edit
        }
        ImGui::EndDisabled();
        return;
    }

    switch (blender.state()) {
        case BlenderState::Probing:
            // A CHILD PROCESS is in flight and there is nothing to offer until it answers. This arm is
            // for Probing ALONE -- see the fall-through group below for why Unknown is not the same
            // thing (code-review B1).
            ImGui::TextDisabled("Checking the Blender version...");
            return;
        case BlenderState::ToolMissing:
            ImGui::TextWrapped("Blender was not found.");
            drawSearchedPaths(blender);
            scratch =
                std::format("Blender {}.{} or newer is recommended; {}.{} is the minimum.", BLENDER_MIN_SUPPORTED.major,
                            BLENDER_MIN_SUPPORTED.minor, BLENDER_ABSOLUTE_MIN.major, BLENDER_ABSOLUTE_MIN.minor);
            ImGui::TextDisabled("%s", scratch.c_str());
            if (ImGui::Button("Locate...")) {
                locateRequested = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Re-detect")) {
                redetectRequested = true;
            }
            return;
        case BlenderState::ToolUnusable:
            scratch = std::format("Blender at '{}' cannot be used.", blender.binaryPath());
            ImGui::TextColored(WARNING_COLOR, "%s", scratch.c_str());
            if (!blender.message().empty()) {
                ImGui::TextWrapped("%s", blender.message().c_str());
            }
            drawBlenderLog(blender, scratch);
            if (ImGui::Button("Locate...")) {
                locateRequested = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Re-detect")) {
                redetectRequested = true;
            }
            return;
        case BlenderState::Converting:
            scratch = std::format("Running Blender... {:.1f} s", static_cast<double>(blender.elapsedSeconds()));
            ImGui::TextUnformatted(scratch.c_str());
            if (ImGui::Button("Cancel")) {
                cancelRequested = true;
            }
            return;
        // code-review B1: `Unknown` is "NOTHING HAS RESOLVED", which is NOT "a probe is running" -- and
        // on a pure CACHE HIT it is the state for the whole session, permanently: resolution is lazy and
        // fires only on a cache MISS (§A-9), so nothing ever leaves Unknown. Grouping it with Probing
        // showed a settled, correctly imported .blend one false sentence ("Checking the Blender
        // version...") and NO controls at all -- no `Re-import`, no `Re-detect`, no `Locate...`, no log
        // node -- with no UI path to force a re-conversion from the task's own headline flow. It belongs
        // here, where the SESSION's own state decides what to offer.
        //
        // What a `Re-import` from here does, stated because it is one click longer than it looks: the
        // request reaches a service that is not yet resolved, so it is DROPPED (AC-30's rule, unchanged)
        // and the session drops to NeedsConversion -- which is exactly the condition EditorApp::tick()'s
        // lazy resolve watches for. The next frame resolves and probes, and the button then converts.
        case BlenderState::Unknown:
        case BlenderState::Ready:
        case BlenderState::Converted:
        case BlenderState::Failed:
            break;  // handled below, where the SESSION's own state decides what to offer
    }

    if (!blender.binaryPath().empty()) {
        scratch = blender.versionString().empty()
                      ? std::format("Blender: {}", blender.binaryPath())
                      : std::format("Blender {} at {}", blender.versionString(), blender.binaryPath());
        ImGui::TextUnformatted(scratch.c_str());
    }
    // D14's "attempt, never refuse" band: a version below the recommended floor still converts, and the
    // warning says so rather than blocking.
    if (blender.state() == BlenderState::Ready && !blender.message().empty()) {
        ImGui::TextColored(WARNING_COLOR, "%s", blender.message().c_str());
    }

    if (session.state() == SessionState::ConversionFailed) {
        if (!blender.message().empty()) {
            ImGui::TextColored(WARNING_COLOR, "%s", blender.message().c_str());
        }
        drawBlenderLog(blender, scratch);
        if (ImGui::Button("Retry")) {
            convertRequested = true;
        }
    } else if (session.state() == SessionState::Imported) {
        // THE ARTIFACT'S OWN provenance, never blender().versionString() (code-review S4). That accessor
        // is the CURRENTLY INSTALLED Blender this session has probed -- empty on a pure cache hit, and,
        // once anything has probed, the version of a binary that did not produce the file on screen. The
        // whole point of the recorded pair is that "convert with 4.2, upgrade to 5.2, and the panel still
        // says 4.2" is TRUE, which is the accepted limitation §A-9 states in those words.
        if (session.artifactBlenderVersion().empty()) {
            scratch = "Imported from a cached Blender export.";
        } else if (session.artifactBlenderPath().empty()) {
            scratch =
                std::format("Imported from a Blender export produced by Blender {}.", session.artifactBlenderVersion());
        } else {
            scratch = std::format("Imported from a Blender export produced by Blender {} at {}.",
                                  session.artifactBlenderVersion(), session.artifactBlenderPath());
        }
        ImGui::TextUnformatted(scratch.c_str());
        drawBlenderLog(blender, scratch);
        if (ImGui::Button("Re-import")) {
            convertRequested = true;
        }
    } else {
        if (ImGui::Button("Import with Blender")) {
            convertRequested = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-detect")) {
        redetectRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Locate...")) {
        locateRequested = true;
    }
}

void drawFailed(const ModelImportSession& session) {
    const ImportResult& result = session.result();
    const std::string statusLine = std::format("Status: {}", importStatusLabel(result.status));
    ImGui::TextColored(WARNING_COLOR, "%s", statusLine.c_str());
    if (!result.message.empty()) {
        ImGui::TextWrapped("%s", result.message.c_str());
    }
    drawWarnings(result);
}

}  // namespace

void ImportDetailsPanel::onDraw(PanelContext& /*context*/) {  // no World/Selection/Project read (D18's
                                                              // "context is ignored" precedent)
    if (sessionPtr == nullptr) {
        // The very first frame of a session's life: EditorApp::tick() reconciles and services the
        // session AFTER drawShellUi (INV-M12), so onDraw() can run once before setSession() ever has.
        // Indistinguishable from Idle, and correctly so -- nothing has been selected yet either way.
        ImGui::TextDisabled("Select an asset in the Assets panel.");
        return;
    }
    // task 3.2.4 (AC-37): FIRST, and BEFORE the switch. Every arm below returns, so a section placed
    // inside one would render in that state only -- and `Re-import` on a cache hit (Imported), `Cancel`
    // mid-run (Converting) and the disabled button on a nil GUID (NeedsConversion) all need it. Gated
    // on the TARGET's name alone, so no other asset type ever shows it and the existing six sections
    // stay exactly six for everything else.
    if (isBlendFileName(sessionPtr->target())) {
        if (ImGui::CollapsingHeader("Blender", ImGuiTreeNodeFlags_DefaultOpen)) {
            drawBlenderSection(*sessionPtr, convertRequested, cancelRequested, locateRequested, redetectRequested,
                               labelScratch);
        }
    }
    switch (sessionPtr->state()) {
        case SessionState::Idle:
            ImGui::TextDisabled("Select an asset in the Assets panel.");
            return;
        case SessionState::NotImportable: {
            labelScratch = std::format("'{}': no importer claims this file type.", sessionPtr->target());
            ImGui::TextWrapped("%s", labelScratch.c_str());
            return;
        }
        case SessionState::Failed:
            drawFailed(*sessionPtr);
            return;
        case SessionState::Imported:
            drawImported(*sessionPtr, applyRequested, revertRequested, editedSettings);
            return;
        case SessionState::NeedsConversion:
        case SessionState::Converting:
        case SessionState::ConversionFailed:
            // task 3.2.4: nothing further. The Blender section drawn ABOVE this switch has already
            // said everything there is to say for these three, and it is drawn for EVERY .blend state
            // (AC-37) rather than from inside an arm -- every arm here returns, so a section placed in
            // one would render in that state only, and `Re-import` on a cache hit (Imported) plus
            // `Cancel` mid-run (Converting) plus the disabled button on a nil GUID (NeedsConversion)
            // all need it. The arms exist because this switch is deliberately `default:`-free.
            return;
    }
    // No `default:` (the importStatusLabel/logAssetScan precedent): every SessionState is handled
    // above, so a future enumerator is a -Wswitch warning, never a silent fall-through.
}

}  // namespace engine::editor

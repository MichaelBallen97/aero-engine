// Aero Engine — task 3.4.1's deliverable: the material system's proof, and three firsts. It is the
// first consumer of a cooked `.ktx2` on a GPU, the first end-to-end read of a `.aeromat`, and the
// first GUID-resolved asset reference — the Phase 5 pak-resolution shape in miniature
// (`.aeromat` guid -> the artifact's AeroSourceGuid -> a GPU texture), which is the chain the GUID
// system was built for at 3.1.1.
//
// What it draws: a 6x6 sphere grid (roughness across, metallic down) under one directional light
// plus ambient with a slow orbit camera; a fully-mapped cube whose five slots come from committed,
// pre-cooked BC1/BC3/BC4/BC5 artifacts; and a double-sided alpha-mask cube that proves the discard
// path and the cull-none pipeline. The RenderView is assembled BY HAND — no World, no
// buildRenderView — because nothing binds a material to a scene yet (3.1.5/3.4.2 own that), which
// makes this file the documentation of "what a caller with materials does".
//
// CI builds this on three OSes (compile-proof only — no display there); run it locally for the
// visual pass and record the result in editor/validation/3.4.1-material-asset-pbr-shader.md.
// Requires AERO_SHADER_TOOLS (the cooked scene.{vert,frag}); without it this compiles a stub main
// that logs and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_MATERIALS_ENABLED
    #include <aero/assets/cooked_texture.hpp>
    #include <aero/reflect/material_format.hpp>
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_MATERIALS_ENABLED

    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers

constexpr std::size_t GRID_N = 6;     // 6x6 = 36 spheres
constexpr float GRID_SPACING = 1.5F;  // world units between sphere centres
constexpr float ORBIT_RADIUS = 9.0F;  // camera distance from the grid centre
constexpr float ORBIT_HEIGHT = 4.0F;  // camera height
constexpr float ORBIT_SPEED = 0.35F;  // rad/s — slow, so GGX highlights are judgeable (D13)
constexpr double TITLE_UPDATE_SECONDS = 0.25;
constexpr double LOG_INTERVAL_SECONDS = 1.0;

// The six committed fixtures, in the order they are loaded and measured. Basenames only: each is
// read as res://materials/textures/<name>.ktx2 through the second VFS mount.
constexpr std::array<std::string_view, 6> FIXTURE_NAMES{"basecolor", "metallic_roughness", "normal",
                                                        "occlusion", "emissive",           "mask_basecolor"};

// The mask cube's base colour, referenced directly rather than through a .aeromat: its material is
// built in code, so this GUID is the one the sample resolves by hand (the pinned table lives in
// README.md).
constexpr std::string_view MASK_BASECOLOR_GUID = "341a0000000000000000000000000006";

// docs/09 §11.4's normative token -> rhi::SamplerDesc table. Consumer-implemented in v1 because the
// sample is the only consumer; when the inspector becomes the second one (3.4.2) whoever writes it
// decides the shared home. A reflect->rhi include would be layer-legal but would couple the JSON
// layer to the GPU layer for six lines of switch.
[[nodiscard]] rhi::AddressMode toAddressMode(MaterialWrap wrap) {
    switch (wrap) {
        case MaterialWrap::Clamp:
            return rhi::AddressMode::ClampToEdge;
        case MaterialWrap::Mirror:
            return rhi::AddressMode::MirroredRepeat;
        case MaterialWrap::Repeat:
            break;
    }
    return rhi::AddressMode::Repeat;
}

[[nodiscard]] rhi::Filter toFilter(MaterialFilter filter) {
    return filter == MaterialFilter::Nearest ? rhi::Filter::Nearest : rhi::Filter::Linear;
}

[[nodiscard]] rhi::SamplerDesc toSamplerDesc(const MaterialTextureSlot& slot) {
    rhi::SamplerDesc desc;
    desc.addressU = toAddressMode(slot.wrapU);
    desc.addressV = toAddressMode(slot.wrapV);
    desc.minFilter = toFilter(slot.minFilter);
    desc.magFilter = toFilter(slot.magFilter);
    switch (slot.mipFilter) {
        case MaterialMipFilter::None:
            // rhi::MipmapMode has no None: the clamp-to-base idiom is Nearest + maxLod 0, which is
            // what §11.4 specifies rather than leaves to taste.
            desc.mipmapMode = rhi::MipmapMode::Nearest;
            desc.maxLod = 0.0F;
            break;
        case MaterialMipFilter::Nearest:
            desc.mipmapMode = rhi::MipmapMode::Nearest;
            break;
        case MaterialMipFilter::Linear:
            desc.mipmapMode = rhi::MipmapMode::Linear;
            break;
    }
    return desc;
}

[[nodiscard]] render::MaterialAlpha toRenderAlpha(MaterialAlphaMode mode) {
    switch (mode) {
        case MaterialAlphaMode::Mask:
            return render::MaterialAlpha::Mask;
        case MaterialAlphaMode::Blend:
            return render::MaterialAlpha::Blend;
        case MaterialAlphaMode::Opaque:
            break;
    }
    return render::MaterialAlpha::Opaque;
}

[[nodiscard]] render::MaterialParams toRenderParams(const MaterialDocument& document) {
    return {.baseColorFactor = document.baseColorFactor,
            .emissiveFactor = document.emissiveFactor,
            .metallicFactor = document.metallicFactor,
            .roughnessFactor = document.roughnessFactor,
            .normalScale = document.normalScale,
            .occlusionStrength = document.occlusionStrength,
            .alpha = toRenderAlpha(document.alphaMode),
            .alphaCutoff = document.alphaCutoff,
            .doubleSided = document.doubleSided};
}

struct LoadedTexture {
    Guid guid;
    rhi::TextureHandle texture;
};

// Read, parse and upload the six committed artifacts, measuring the wall time of the whole set
// (R4/D17: uploadTexture is a full device stall per call, which this task ACCEPTS and MEASURES
// rather than assumes away — validation row 9 reads its number from the INFO line below).
[[nodiscard]] std::optional<std::vector<LoadedTexture>> loadCookedTextures(rhi::Device& device,
                                                                           const VirtualFileSystem& vfs) {
    std::vector<LoadedTexture> loaded;
    loaded.reserve(FIXTURE_NAMES.size());
    const double startedAt = monotonicSeconds();
    for (const std::string_view name : FIXTURE_NAMES) {
        const std::string path = "res://materials/textures/" + std::string(name) + ".ktx2";
        const std::optional<ByteBuffer> bytes = vfs.readFile(path);
        if (!bytes) {
            AERO_LOG_CRITICAL("phase-3-materials: could not read {}", path);
            return std::nullopt;
        }
        const assets::CookedTextureParse parse = assets::parseCookedTexture(*bytes);
        if (parse.status != assets::CookedTextureStatus::Ok) {
            AERO_LOG_CRITICAL("phase-3-materials: {} is not a readable cooked texture: {}", path,
                              assets::cookedTextureStatusLabel(parse.status));
            return std::nullopt;
        }
        const rhi::TextureHandle texture = render::createTextureFromCookedTexture(device, parse.view);
        if (!texture.valid()) {
            AERO_LOG_CRITICAL("phase-3-materials: {} could not be uploaded", path);
            return std::nullopt;
        }
        loaded.push_back({parse.view.sourceGuid(), texture});
    }
    const double elapsedMs = (monotonicSeconds() - startedAt) * 1000.0;
    AERO_LOG_INFO("phase-3-materials: {} cooked textures uploaded in {:.1f} ms (mean {:.1f} ms)", loaded.size(),
                  elapsedMs, elapsedMs / static_cast<double>(loaded.size()));
    return loaded;
}

// Linear scan over six entries — no map, deliberately: this IS the pak-resolution shape, and at six
// entries a map would be ceremony. An unresolvable GUID is a hard startup error naming it, never a
// silent fallback to the default texture: a material that references an asset which is not there is
// a broken project, not a styling choice.
[[nodiscard]] rhi::TextureHandle findTexture(const std::vector<LoadedTexture>& loaded, Guid guid) {
    for (const LoadedTexture& entry : loaded) {
        if (entry.guid == guid) {
            return entry.texture;
        }
    }
    AERO_LOG_CRITICAL("phase-3-materials: no loaded texture carries source GUID {}", formatGuid(guid));
    return {};
}

// v1 honours UV set 0 only (MeshVertex carries one set); a non-zero set is stored by the format for
// fidelity and WARNed here, once per run.
bool warnedUvSetOnce = false;

[[nodiscard]] std::optional<render::MaterialTextureSlot> resolveSlot(const std::optional<MaterialTextureSlot>& slot,
                                                                     const std::vector<LoadedTexture>& loaded) {
    if (!slot) {
        return render::MaterialTextureSlot{};  // absent: the built-in identity default covers it
    }
    if (slot->uvSet != 0 && !warnedUvSetOnce) {
        AERO_LOG_WARN(
            "phase-3-materials: uvSet {} is stored but not honoured — v1 samples set 0 only; "
            "this warning latches once",
            slot->uvSet);
        warnedUvSetOnce = true;
    }
    const rhi::TextureHandle texture = findTexture(loaded, slot->guid);
    if (!texture.valid()) {
        return std::nullopt;
    }
    return render::MaterialTextureSlot{.texture = texture, .sampler = toSamplerDesc(*slot)};
}

// The normal matrix scene_render computes for every instance, spelled the same way rather than
// assumed to be the identity: these transforms are translation-only today, and a future rotated
// instance must not need this line rewritten.
[[nodiscard]] Mat4 normalMatrixOf(const Mat4& model) {
    const Mat3 m = transpose(inverse(toMat3(model)));
    return Mat4{std::array<Vec4, 4>{Vec4{m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0F},
                                    Vec4{m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0F},
                                    Vec4{m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0F},
                                    Vec4{0.0F, 0.0F, 0.0F, 1.0F}}};
}

[[nodiscard]] Mat4 translation(Vec3 position) {
    Mat4 m = Mat4::identity();
    m.columns[3] = Vec4{position.x, position.y, position.z, 1.0F};
    return m;
}

int runSample() {
    platform::Context ctx;  // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 3 Materials", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }

    std::optional<rhi::Device> device = rhi::Device::create();
    if (!device) {
        AERO_LOG_CRITICAL("no GPU device");
        return 1;
    }

    std::optional<render::Renderer> renderer = render::Renderer::create(*device, *window, {.depth = true});
    if (!renderer) {
        AERO_LOG_CRITICAL("renderer creation failed");
        return 1;
    }

    // Two mounts on ONE VirtualFileSystem: the cooked shaders at the res:// root (ForwardRenderer's
    // res://scene.* defaults) and this sample's committed fixtures under res://materials/. Mounts are
    // searched most-recent-first and never collide here — the prefixes are disjoint. The sub-prefix
    // is spelled in FULL virtual form ("res://materials"): mount() normalizes it through the same
    // res:// parser every read uses, so a bare "materials" is unaddressable and mounts nothing.
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    vfs.mount("res://materials", std::make_unique<DirectoryBackend>(AERO_PHASE3_MATERIALS_DIR));

    std::optional<render::ForwardRenderer> forward = render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = renderer->colorFormat(), .depthFormat = renderer->depthFormat()});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    const std::optional<std::vector<LoadedTexture>> textures = loadCookedTextures(*device, vfs);
    if (!textures) {
        return 1;
    }

    // --- the mapped cube: its material comes from the committed .aeromat, end to end -------------
    const std::optional<std::string> materialText = vfs.readText("res://materials/mapped_cube.aeromat");
    if (!materialText) {
        AERO_LOG_CRITICAL("phase-3-materials: could not read res://materials/mapped_cube.aeromat");
        return 1;
    }
    const MaterialParseResult parsed = parseMaterial(*materialText);
    if (!parsed.ok()) {
        AERO_LOG_CRITICAL("phase-3-materials: mapped_cube.aeromat rejected: {} (line {}, column {})",
                          parsed.error.message, parsed.error.line, parsed.error.column);
        return 1;
    }
    const MaterialDocument& document = *parsed.document;
    AERO_LOG_INFO("phase-3-materials: loaded material \"{}\" from mapped_cube.aeromat", document.name);

    render::MaterialTextureSlots mappedSlots;
    const std::array<const std::optional<MaterialTextureSlot>*, 5> documentSlots{
        &document.baseColor, &document.metallicRoughness, &document.normal, &document.occlusion, &document.emissive};
    const std::array<render::MaterialTextureSlot*, 5> targets{&mappedSlots.baseColor, &mappedSlots.metallicRoughness,
                                                              &mappedSlots.normal, &mappedSlots.occlusion,
                                                              &mappedSlots.emissive};
    for (std::size_t i = 0; i < documentSlots.size(); ++i) {
        const std::optional<render::MaterialTextureSlot> resolved = resolveSlot(*documentSlots[i], *textures);
        if (!resolved) {
            return 1;  // resolveSlot has already named the unresolvable GUID
        }
        *targets[i] = *resolved;
    }
    const render::MaterialHandle mappedMaterial = forward->createMaterial(toRenderParams(document), mappedSlots);

    // --- the mask cube: built in code, resolving its one GUID directly ---------------------------
    const std::optional<Guid> maskGuid = parseGuid(MASK_BASECOLOR_GUID);
    if (!maskGuid) {
        AERO_LOG_CRITICAL("phase-3-materials: the mask cube's pinned GUID literal is malformed");
        return 1;
    }
    const rhi::TextureHandle maskTexture = findTexture(*textures, *maskGuid);
    if (!maskTexture.valid()) {
        return 1;
    }
    render::MaterialTextureSlots maskSlots;
    maskSlots.baseColor.texture = maskTexture;
    const render::MaterialHandle maskMaterial = forward->createMaterial(
        {.alpha = render::MaterialAlpha::Mask, .alphaCutoff = 0.5F, .doubleSided = true}, maskSlots);

    // --- the grid: 36 untextured materials, roughness across and metallic down --------------------
    // Default-white and untextured on purpose: the built-in 1x1 identity defaults cover BOTH RGBA8
    // formats, so the grid is what proves them while the two cubes prove the block families.
    std::vector<render::MaterialHandle> gridMaterials;
    gridMaterials.reserve(GRID_N * GRID_N);
    for (std::size_t row = 0; row < GRID_N; ++row) {
        for (std::size_t col = 0; col < GRID_N; ++col) {
            const float metallic = static_cast<float>(row) / static_cast<float>(GRID_N - 1);
            const float roughness = static_cast<float>(col) / static_cast<float>(GRID_N - 1);
            gridMaterials.push_back(forward->createMaterial(
                {.baseColorFactor = Vec4::one(), .metallicFactor = metallic, .roughnessFactor = roughness}, {}));
        }
    }

    // --- the instance list: 36 spheres + the two cubes, rebuilt per frame for the orbit ----------
    std::vector<render::MeshInstance> instances(gridMaterials.size() + 2);
    const float half = static_cast<float>(GRID_N - 1) * 0.5F;
    for (std::size_t row = 0; row < GRID_N; ++row) {
        for (std::size_t col = 0; col < GRID_N; ++col) {
            const std::size_t index = (row * GRID_N) + col;
            render::MeshInstance& instance = instances[index];
            instance.primitive = render::PrimitiveId::Sphere;
            instance.model = translation({(static_cast<float>(col) - half) * GRID_SPACING, 0.0F,
                                          (static_cast<float>(row) - half) * GRID_SPACING});
            instance.normalMatrix = normalMatrixOf(instance.model);
            instance.material = gridMaterials[index];
        }
    }
    // The one deliberately tinted instance: MeshRenderer.color keeps its exact pre-3.4.1 meaning and
    // multiplies baseColorFactor.rgb exactly once, which this corner sphere is the witness for.
    instances[0].color = Vec3{1.0F, 0.4F, 0.4F};

    render::MeshInstance& mappedCube = instances[gridMaterials.size()];
    mappedCube.primitive = render::PrimitiveId::Cube;
    mappedCube.model = translation({-1.2F, 1.8F, 0.0F});
    mappedCube.normalMatrix = normalMatrixOf(mappedCube.model);
    mappedCube.material = mappedMaterial;

    render::MeshInstance& maskCube = instances[gridMaterials.size() + 1];
    maskCube.primitive = render::PrimitiveId::Cube;
    maskCube.model = translation({1.2F, 1.8F, 0.0F});
    maskCube.normalMatrix = normalMatrixOf(maskCube.model);
    maskCube.material = maskMaterial;

    FrameClock clock;
    double lastTitleAt = 0.0;
    double lastLogAt = 0.0;
    bool running = true;
    while (running) {
        ctx.newFrame();
        platform::Event ev;
        while (ctx.pollEvent(ev)) {
            if (ev.type == platform::EventType::Quit || ev.type == platform::EventType::WindowClose) {
                running = false;
            }
        }
        if (ctx.input().keyDown(platform::Key::Escape)) {
            running = false;
        }
        clock.tick();

        const auto angle = static_cast<float>(clock.totalSeconds()) * ORBIT_SPEED;
        const Vec3 eye{ORBIT_RADIUS * std::cos(angle), ORBIT_HEIGHT, ORBIT_RADIUS * std::sin(angle)};

        const rhi::Color sky{0.02F, 0.02F, 0.03F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            // The aspect comes from the OPEN FRAME (phase-0-cube's own idiom), which is what makes a
            // drag-resize track without stretching — Renderer has no extent() of its own.
            const rhi::Extent2D extent = frame->extent();
            const float aspect =
                extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);
            render::RenderView view;
            view.camera = {lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
                           perspective(radians(60.0F), aspect, 0.1F, 100.0F), eye};
            view.directional = {
                .direction = normalize(Vec3{-0.5F, -1.0F, -0.3F}), .color = Vec3::one(), .intensity = 3.0F};
            // task E.2.1: Flat at the sample's own constant, intensity 1 -- byte-identical shade.
            view.environment = {.ambientMode = render::AmbientMode::Flat,
                                .ambientColor = Vec3{0.03F, 0.03F, 0.03F},
                                .ambientIntensity = 1.0F};
            for (render::MeshInstance& instance : instances) {
                instance.mvp = view.camera.proj * view.camera.view * instance.model;
            }
            view.instances = instances;

            forward->draw(*frame, view);
            if (renderer->endFrame(std::move(*frame))) {
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    const std::string fps = std::to_string(std::lround(clock.fps()));
                    window->setTitle("Aero — Phase 3 Materials · " + fps + " fps");
                    lastTitleAt = now;
                }
                if (now - lastLogAt >= LOG_INTERVAL_SECONDS) {
                    AERO_LOG_INFO("fps {:.1f} · dt {:.2f} ms", clock.fps(), clock.deltaSeconds() * 1000.0F);
                    lastLogAt = now;
                }
            }
        }
    }

    // The renderer BORROWS its materials' textures — creating them was this file's job and so is
    // destroying them (material.hpp's ownership note). The ForwardRenderer's own destructor releases
    // the pipelines, samplers and built-in defaults.
    for (const LoadedTexture& entry : *textures) {
        device->destroyTexture(entry.texture);
    }

    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.4.1-material-asset-pbr-shader.md (this OS)");
    return 0;
}

}  // namespace

int main() {
    try {
        return runSample();
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-materials: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-materials: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_MATERIALS_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-materials needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_MATERIALS_ENABLED

#include "Scene/Systems/RenderSystem.h"
#include "Scene/Systems/TransformSystem.h"

#include "Assets/AssetManager.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Logger.h"
#include "Graphics/Frustum.h"
#include "Graphics/OcclusionCulling.h"

#include <glm/geometric.hpp>
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace engine {
namespace {

// Stable material identity for an asset id (FNV-1a). Until the asset
// system assigns real material slots, this groups identical asset ids
// under identical sort keys so their draws stay adjacent.
uint32_t materialIdFor(std::string_view assetId) {
    uint32_t hash = 2166136261u;
    for (const unsigned char c : assetId) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

// Evaluates one entity: model matrix, world bounds, frustum test, and
// the render-queue sort data (material + view depth). Components are
// fetched by entity handle through the view (read-only sparse-array
// lookups into dense storage); writes go to a disjoint output slot, so
// ranges are safe to run concurrently.
// Largest world-space stretch of a model matrix (max basis length):
// scales a local bounding sphere into world space.
float maxBasisScale(const glm::mat4& model) {
    return std::max({glm::length(glm::vec3{model[0]}), glm::length(glm::vec3{model[1]}),
                     glm::length(glm::vec3{model[2]})});
}

template <typename View>
void evaluateMesh(const View& view, entt::entity entity, const Frustum& frustum, const glm::mat4& viewMatrix,
                  const glm::mat4& viewProjection, bool cullingEnabled, const AssetManager* assets,
                  const glm::mat4& model, const glm::vec3& worldPosition,
                  const std::vector<AABB>& occluders, bool isOccluder, std::optional<DrawItem>& outItem,
                  char& outCulled, char& outOccluded) {
    const MeshComponent& mesh = view.template get<MeshComponent>(entity);
    outCulled = 0;
    outOccluded = 0;
    const bool occlusionActive = !occluders.empty() && !isOccluder;
    AABB worldBounds;
    bool haveWorldBounds = false;
    if (cullingEnabled || occlusionActive) {
        worldBounds = mesh.localBounds.transformed(model);
        haveWorldBounds = true;
    }
    if (cullingEnabled) {
        // Cheap bounding-sphere reject first (one plane test set
        // against a sphere), then the exact p-vertex AABB test.
        const glm::vec3 sphereCenter = glm::vec3{model * glm::vec4{mesh.localBounds.center(), 1.0f}};
        const float sphereRadius = glm::length(mesh.localBounds.extents()) * maxBasisScale(model);
        if (!frustum.intersectsSphere(sphereCenter, sphereRadius) || !frustum.intersects(worldBounds)) {
            outCulled = 1;
            return;
        }
    }
    if (occlusionActive && haveWorldBounds && isOccludedByAny(viewProjection, occluders, worldBounds)) {
        outOccluded = 1;
        return;
    }
    const float viewDepth = -(viewMatrix * glm::vec4(worldPosition, 1.0f)).z;
    // Stable material identity: the AssetManager's registered id (and
    // its dense descriptor index) when available; the deterministic
    // name hash is the fallback for scenes built without an asset pass.
    uint32_t materialId = materialIdFor(mesh.materialName);
    uint32_t descriptorIndex = 0;
    glm::vec4 baseColor{1.0f};
    // Default lighting matches MaterialLighting's defaults, so scenes
    // built without an asset pass still shade with the full model.
    glm::vec4 specular{0.35f, 0.35f, 0.35f, 32.0f};
    glm::vec4 surface{0.35f, 0.10f, 3.0f, 0.28f};
    glm::vec4 pbrParams{0.0f, 0.55f, 1.0f, 0.0f};
    std::array<int32_t, 4> textureIndices{-1, -1, -1, -1};
    if (assets != nullptr) {
        if (const MaterialAsset* material = assets->material(mesh.materialName)) {
            materialId = material->id;
            descriptorIndex = material->descriptorIndex;
            baseColor = material->desc.baseColor;
            const MaterialLighting& lighting = material->desc.lighting;
            specular = glm::vec4{lighting.specularColor, lighting.shininess};
            surface = glm::vec4{lighting.specularStrength, lighting.rimStrength, lighting.rimPower,
                                lighting.ambientStrength};
            const MaterialPbr& pbr = material->desc.pbr;
            pbrParams = glm::vec4{pbr.metallic, pbr.roughness, pbr.occlusionStrength, 0.0f};
            // Dense texture slots (id - 1) into the renderer's
            // material texture array; unknown/oversized stays -1.
            const auto resolveTexture = [assets](const std::string& name) -> int32_t {
                if (name.empty()) {
                    return -1;
                }
                const TextureAsset* texture = assets->texture(name);
                if (texture == nullptr || texture->id == kInvalidAssetId ||
                    texture->id - 1 >= kMaxMaterialTextures) {
                    return -1;
                }
                return static_cast<int32_t>(texture->id - 1);
            };
            textureIndices = {resolveTexture(pbr.albedoTexture), resolveTexture(pbr.metallicRoughnessTexture),
                              resolveTexture(pbr.normalTexture), resolveTexture(pbr.occlusionTexture)};
        }
    }
    DrawItem item;
    item.model = model;
    item.materialId = materialId;
    item.descriptorIndex = descriptorIndex;
    item.baseColor = baseColor;
    item.specular = specular;
    item.surface = surface;
    item.pbrParams = pbrParams;
    item.textureIndices = textureIndices;
    item.viewDepth = viewDepth;
    item.sortKey = RenderQueue::makeSortKey(materialId, viewDepth);
    outItem = item;
}

} // namespace

RenderSystem::RenderSystem(Renderer& renderer, const Swapchain& swapchain, JobSystem* jobs,
                           bool frustumCulling, size_t parallelMinItems, bool sortDrawItems,
                           const AssetManager* assets, bool occlusionCulling)
    : m_renderer(renderer), m_swapchain(swapchain), m_jobs(jobs), m_frustumCulling(frustumCulling),
      m_occlusionCulling(occlusionCulling), m_parallelMinItems(parallelMinItems),
      m_sortDrawItems(sortDrawItems), m_assets(assets) {}

PreparedFrame RenderSystem::prepareFrame(Scene& scene, float aspectRatio,
                                         const FramePrepareOptions& options) {
    PreparedFrame frame;

    const CameraComponent* activeCamera = scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return frame;
    }
    frame.hasCamera = true;
    frame.view = activeCamera->camera.viewMatrix();
    frame.projection = activeCamera->camera.projectionMatrix(aspectRatio);

    // Collect visible entities from the view. Entity handles come from
    // the view's dense entity array, so this snapshot is one contiguous
    // int array — cheap, and immune to storage reallocation afterwards.
    auto view = scene.registry().view<TransformComponent, MeshComponent>();
    std::vector<entt::entity> entities;
    for (const entt::entity entity : view) {
        if (view.get<MeshComponent>(entity).visible) {
            entities.push_back(entity);
        }
    }
    frame.meshCount = static_cast<uint32_t>(entities.size());

    // World placement per entity (Phase 18): the TransformSystem-
    // propagated matrix when present, the local model matrix
    // otherwise. Resolved serially here so the parallel evaluation
    // below only reads finished matrices.
    std::vector<glm::mat4> worldMatrices(entities.size());
    std::vector<glm::vec3> worldPositions(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        worldMatrices[i] = TransformSystem::effectiveWorldMatrix(
            view.get<TransformComponent>(entities[i]),
            scene.registry().try_get<WorldTransformComponent>(entities[i]));
        worldPositions[i] = glm::vec3{worldMatrices[i][3]};
    }

    // Occlusion culling (Phase 21): world boxes of static box/OBB
    // colliders act as occluders for the conservative screen-space
    // test. Occluder entities themselves are never occlusion-culled.
    const glm::mat4 viewProjection = frame.projection * frame.view;
    std::vector<AABB> occluderBoxes;
    std::vector<char> occluderFlags(entities.size(), 0);
    if (options.occlusionCulling) {
        std::unordered_map<uint32_t, size_t> entityIndex;
        entityIndex.reserve(entities.size());
        for (size_t i = 0; i < entities.size(); ++i) {
            entityIndex[entt::to_integral(entities[i])] = i;
        }
        const auto colliderView =
            scene.registry().view<TransformComponent, RigidBodyComponent, ColliderComponent>();
        for (const entt::entity entity : colliderView) {
            const RigidBodyComponent& body = colliderView.get<RigidBodyComponent>(entity);
            const ColliderComponent& collider = colliderView.get<ColliderComponent>(entity);
            if (!body.isStatic ||
                (collider.type != ColliderComponent::Type::Box && collider.type != ColliderComponent::Type::Obb)) {
                continue;
            }
            const glm::mat4 world = TransformSystem::effectiveWorldMatrix(
                colliderView.get<TransformComponent>(entity),
                scene.registry().try_get<WorldTransformComponent>(entity));
            const AABB localBox{collider.centerOffset - glm::abs(collider.halfExtents),
                                collider.centerOffset + glm::abs(collider.halfExtents)};
            occluderBoxes.push_back(localBox.transformed(world));
            if (const auto found = entityIndex.find(entt::to_integral(entity)); found != entityIndex.end()) {
                occluderFlags[found->second] = 1;
            }
        }
    }

    frame.lighting = LightingSystem::gather(scene, aspectRatio);

    const Frustum frustum{viewProjection};
    std::vector<std::optional<DrawItem>> evaluated(entities.size());
    std::vector<char> culledFlags(entities.size(), 0); // not vector<bool>: slots written concurrently
    std::vector<char> occludedFlags(entities.size(), 0);

    const auto evaluateRange = [&view, &entities, &frustum, &options, &evaluated, &culledFlags, &frame,
                                &worldMatrices, &worldPositions, &viewProjection, &occluderBoxes,
                                &occluderFlags, &occludedFlags](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            evaluateMesh(view, entities[i], frustum, frame.view, viewProjection, options.frustumCulling,
                         options.assets, worldMatrices[i], worldPositions[i], occluderBoxes,
                         occluderFlags[i] != 0, evaluated[i], culledFlags[i], occludedFlags[i]);
        }
    };

    if (options.jobs != nullptr && entities.size() >= options.parallelMinItems) {
        // parallelFor returning is the synchronization barrier: every slot
        // below is fully written before compaction reads it.
        options.jobs->parallelFor(entities.size(), 0, evaluateRange);
    } else {
        evaluateRange(0, entities.size());
    }

    // Sequential compaction into the render queue (deterministic base
    // order), then the queue sort: material groups first for minimal
    // state changes, front-to-back inside each group for Early-Z.
    frame.renderQueue.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        if (culledFlags[i] != 0) {
            ++frame.culledCount;
        } else if (occludedFlags[i] != 0) {
            ++frame.occludedCount;
        } else if (evaluated[i].has_value()) {
            frame.renderQueue.push(*evaluated[i]);
        }
    }
    if (options.sortDrawItems) {
        frame.renderQueue.sort(options.jobs, options.parallelMinItems);
    }
    return frame;
}

bool RenderSystem::render(Scene& scene) {
    const VkExtent2D extent = m_swapchain.extent();
    const float aspectRatio = extent.height > 0
                                  ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                                  : 1.0f;

    FramePrepareOptions options;
    options.jobs = m_jobs;
    options.assets = m_assets;
    options.frustumCulling = m_frustumCulling;
    options.occlusionCulling = m_occlusionCulling;
    options.parallelMinItems = m_parallelMinItems;
    options.sortDrawItems = m_sortDrawItems;

    const PreparedFrame frame = prepareFrame(scene, aspectRatio, options);
    if (!frame.hasCamera) {
        if (!m_warnedNoCamera) {
            m_warnedNoCamera = true;
            ENGINE_LOG_WARNING("RenderSystem: scene has no active camera; skipping rendering "
                               "until one is added");
        }
        m_lastDrawnCount = 0;
        m_lastCulledCount = 0;
        return false;
    }
    m_warnedNoCamera = false;
    m_lastDrawnCount = static_cast<uint32_t>(frame.renderQueue.size());
    m_lastCulledCount = frame.culledCount;
    m_lastOccludedCount = frame.occludedCount;

    if (m_assets != nullptr) {
        // Mirror the CPU texture table into the GPU material array;
        // a no-op unless the registry version moved (Phase 22).
        std::vector<MaterialTextureUpload> textureUploads;
        const std::vector<TextureAsset>& textures = m_assets->textures();
        textureUploads.reserve(textures.size());
        for (const TextureAsset& texture : textures) {
            textureUploads.push_back(MaterialTextureUpload{
                texture.width, texture.height,
                std::span<const uint8_t>{texture.pixels.data(), texture.pixels.size()}});
        }
        m_renderer.syncMaterialTextures(textureUploads, m_assets->textureRegistryVersion());
    }

    FrameLighting frameLighting;
    frameLighting.cascadeCount = frame.lighting.cascadeCount;
    frameLighting.cascadesFilled = frame.lighting.cascadesFilled;
    frameLighting.cascadeSplits = frame.lighting.cascadeSplits;
    frameLighting.cascades = frame.lighting.cascadeMatrices;
    frameLighting.lightCounts = frame.lighting.lightCounts;
    frameLighting.lightPositionRange = frame.lighting.lightPositionRange;
    frameLighting.lightColorIntensity = frame.lighting.lightColorIntensity;
    frameLighting.lightDirectionType = frame.lighting.lightDirectionType;
    frameLighting.lightCone = frame.lighting.lightCone;
    frameLighting.directionalDirection = frame.lighting.directionalDirection;
    frameLighting.directionalRadiance = frame.lighting.directionalRadiance;
    frameLighting.directionalColor = frame.lighting.directionalColor;
    frameLighting.directionalCastShadow = frame.lighting.directionalCastShadow;
    frameLighting.spotShadowEnabled = frame.lighting.spotShadowEnabled;
    frameLighting.spotShadowViewProj = frame.lighting.spotShadowViewProj;
    frameLighting.spotShadowLightIndex = frame.lighting.spotShadowLightIndex;
    return m_renderer.drawFrame(frame.view, frame.projection, frame.renderQueue.items(), frameLighting);
}

} // namespace engine

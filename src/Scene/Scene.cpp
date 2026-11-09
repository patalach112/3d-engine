#include "Scene/Scene.h"

#include "Core/Logger.h"

namespace engine {

Entity Scene::createEntity(const std::string& name) {
    Entity entity{m_registry.create(), this};
    ++m_entityCount;
    entity.addComponent<TagComponent>(TagComponent{name});
    ENGINE_LOG_DEBUG("Scene: created entity '" << name << "' (id " << entity.id() << ")");
    return entity;
}

Entity Scene::spawnImportedModel(const MeshAsset& asset, const std::string& name, const glm::vec3& position,
                                 const glm::vec3& userScale, float mass) {
    if (asset.id == kInvalidAssetId || asset.vertices.empty() || asset.indices.empty()) {
        ENGINE_LOG_WARNING("Scene: spawnImportedModel('" << name << "') rejected: asset '" << asset.name
                                                         << "' has no geometry");
        return Entity{};
    }

    Entity entity = createEntity(name);

    // Auto-scale normalization: raw file units cancel out here. The
    // same factor scales the collider (PhysicsSystem multiplies local
    // collider dimensions by transform scale), so mesh and hitbox stay
    // locked together at every user scale.
    auto& transform = entity.addComponent<TransformComponent>(position);
    transform.scale = userScale * asset.bounds.autoScaleFactor;

    auto& mesh = entity.addComponent<MeshComponent>(asset.name);
    mesh.localBounds = asset.bounds.aabb();

    entity.addComponent<RigidBodyComponent>(mass);

    // 1:1 hitbox: raw AABB half-extents as the box size, raw AABB
    // center as the local offset from the model origin.
    entity.addComponent<ColliderComponent>(ColliderComponent::box(asset.bounds.extents, asset.bounds.center));
    return entity;
}

Entity Scene::spawnImportedModel(const MeshAsset& asset, const std::string& name, const glm::vec3& position,
                                 float userScale, float mass) {
    return spawnImportedModel(asset, name, position, glm::vec3{userScale}, mass);
}

void Scene::destroyEntity(Entity entity) {
    if (!isValid(entity)) {
        return;
    }
    ENGINE_LOG_DEBUG("Scene: destroying entity '" << entity.name() << "' (id " << entity.id() << ")");
    m_registry.destroy(entity.handle());
    --m_entityCount;
}

void Scene::reserveCapacity(size_t entityCount) {
    if (entityCount == 0) {
        return;
    }
    m_registry.storage<entt::entity>().reserve(entityCount);
    m_registry.storage<TransformComponent>().reserve(entityCount);
    m_registry.storage<MeshComponent>().reserve(entityCount);
    m_registry.storage<CameraComponent>().reserve(entityCount);
    m_registry.storage<SpinComponent>().reserve(entityCount);
    m_registry.storage<RigidBodyComponent>().reserve(entityCount);
    m_registry.storage<ColliderComponent>().reserve(entityCount);
    m_registry.storage<GravityFieldComponent>().reserve(entityCount);
    m_registry.storage<JointComponent>().reserve(entityCount);
    m_registry.storage<ThrusterComponent>().reserve(entityCount);
    ENGINE_LOG_DEBUG("Scene: reserved component storage for " << entityCount << " entities");
}

bool Scene::isValid(Entity entity) const {
    return entity.handle() != entt::null && m_registry.valid(entity.handle());
}

CameraComponent* Scene::findActiveCamera() {
    CameraComponent* active = nullptr;
    m_registry.view<CameraComponent>().each([&active](CameraComponent& camera) {
        if (active == nullptr && camera.isActive) {
            active = &camera;
        }
    });
    return active;
}

const CameraComponent* Scene::findActiveCamera() const {
    const CameraComponent* active = nullptr;
    m_registry.view<CameraComponent>().each([&active](const CameraComponent& camera) {
        if (active == nullptr && camera.isActive) {
            active = &camera;
        }
    });
    return active;
}

entt::entity Scene::findActiveCameraEntity() const {
    entt::entity active = entt::null;
    m_registry.view<CameraComponent>().each([&active](entt::entity entity, const CameraComponent& camera) {
        if (active == entt::null && camera.isActive) {
            active = entity;
        }
    });
    return active;
}

} // namespace engine

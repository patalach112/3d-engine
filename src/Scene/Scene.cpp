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

} // namespace engine

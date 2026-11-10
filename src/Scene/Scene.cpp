#include "Scene/Scene.h"

#include "Core/Logger.h"
#include "Physics/PhysicsComponents.h"
#include "Scene/Systems/TransformSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace engine {
namespace {

// Creates the storage for each listed component type (a no-op where
// one already exists). Called once per Scene from engine code so
// every built-in storage is host-owned for the scene's whole
// lifetime — see the module-boundary contract on Scene's constructor.
template <typename... Components>
void primeComponentStorages(Scene::Registry& registry) {
    (registry.storage<Components>(), ...);
}

} // namespace

Scene::Scene() {
    primeComponentStorages<TagComponent, TransformComponent, ParentComponent, ChildrenComponent,
                           WorldTransformComponent, CameraComponent, PointLightComponent, SpotLightComponent,
                           DirectionalLightComponent, MeshComponent, NativeScriptComponent, SpinComponent, RigidBodyComponent,
                           ColliderComponent, GravityFieldComponent, JointComponent, ThrusterComponent>(
        m_registry);
}

Scene::~Scene() {
    // Destroy every entity through the sink-connected path (see
    // the lifetime contract on the declaration). Note this must
    // NOT go through Scene::destroyEntity: that publishes on the
    // event bus and unlinks hierarchy bookkeeping, neither of
    // which may be touched while the scene is half-destroyed.
    // registry.clear() runs each pool's pop_all(), which is what
    // publishes the destruction sinks.
    m_registry.clear();
    m_entityCount = 0;
}

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
    const entt::entity handle = entity.handle();

    // Hierarchy bookkeeping (Phase 18): detach from the parent's
    // child list, then apply the destroy mode to this entity's
    // children before its own destruction.
    unlinkFromParent(handle);
    if (m_registry.all_of<ParentComponent>(handle)) {
        m_registry.remove<ParentComponent>(handle);
    }
    if (ChildrenComponent* children = m_registry.try_get<ChildrenComponent>(handle);
        children != nullptr && !children->children.empty()) {
        // Copied: child handlers mutate the original list.
        const std::vector<entt::entity> childHandles = children->children;
        if (m_hierarchyDestroyMode == HierarchyDestroyMode::Cascade) {
            for (const entt::entity childHandle : childHandles) {
                destroyEntity(Entity{childHandle, this});
            }
        } else {
            // Orphan: each child becomes a root whose local transform
            // is its current world transform, so nothing visually
            // moves when the parent disappears.
            for (const entt::entity childHandle : childHandles) {
                if (!m_registry.valid(childHandle)) {
                    continue;
                }
                if (TransformComponent* childTransform = m_registry.try_get<TransformComponent>(childHandle);
                    childTransform != nullptr) {
                    glm::vec3 position;
                    glm::vec3 scale;
                    glm::quat orientation;
                    TransformSystem::decompose(computeWorldMatrix(childHandle), position, orientation, scale);
                    childTransform->position = position;
                    childTransform->scale = scale;
                    childTransform->setOrientation(orientation);
                }
                if (m_registry.all_of<ParentComponent>(childHandle)) {
                    m_registry.remove<ParentComponent>(childHandle);
                }
            }
        }
        children->children.clear();
    }

    if (m_eventBus != nullptr) {
        m_eventBus->publish(EntityDestroyedEvent{handle});
    }
    m_registry.destroy(handle);
    --m_entityCount;
}

glm::mat4 Scene::computeWorldMatrix(entt::entity entity) const {
    std::vector<entt::entity> chain;
    entt::entity current = entity;
    for (int depth = 0; depth < 1024 && current != entt::null && m_registry.valid(current); ++depth) {
        chain.push_back(current);
        const ParentComponent* parent = m_registry.try_get<ParentComponent>(current);
        current = parent != nullptr ? parent->parent : entt::null;
    }
    glm::mat4 world{1.0f};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const TransformComponent* transform = m_registry.try_get<TransformComponent>(*it);
            transform != nullptr) {
            world = world * transform->modelMatrix();
        }
    }
    return world;
}

void Scene::unlinkFromParent(entt::entity child) {
    const ParentComponent* parentComponent = m_registry.try_get<ParentComponent>(child);
    if (parentComponent == nullptr || parentComponent->parent == entt::null) {
        return;
    }
    if (ChildrenComponent* siblings = m_registry.try_get<ChildrenComponent>(parentComponent->parent);
        siblings != nullptr) {
        std::vector<entt::entity>& list = siblings->children;
        list.erase(std::remove(list.begin(), list.end(), child), list.end());
    }
}

bool Scene::isDescendantOf(Entity entity, Entity ancestor) const {
    if (!isValid(entity) || !isValid(ancestor)) {
        return false;
    }
    entt::entity current = entity.handle();
    for (int depth = 0; depth < 1024 && current != entt::null && m_registry.valid(current); ++depth) {
        if (current == ancestor.handle()) {
            return true;
        }
        const ParentComponent* parent = m_registry.try_get<ParentComponent>(current);
        current = parent != nullptr ? parent->parent : entt::null;
    }
    return false;
}

bool Scene::setParent(Entity child, Entity parent, bool keepWorldTransform) {
    if (!isValid(child) || !isValid(parent) || child.handle() == parent.handle()) {
        return false;
    }
    if (isDescendantOf(parent, child)) {
        return false; // would close a cycle
    }
    const entt::entity childHandle = child.handle();
    const entt::entity parentHandle = parent.handle();

    glm::mat4 childWorld{1.0f};
    if (keepWorldTransform) {
        childWorld = computeWorldMatrix(childHandle);
    }

    unlinkFromParent(childHandle);
    m_registry.emplace_or_replace<ParentComponent>(childHandle, ParentComponent{parentHandle});
    ChildrenComponent& siblings = m_registry.get_or_emplace<ChildrenComponent>(parentHandle);
    if (std::find(siblings.children.begin(), siblings.children.end(), childHandle) == siblings.children.end()) {
        siblings.children.push_back(childHandle);
    }

    if (keepWorldTransform) {
        if (TransformComponent* transform = m_registry.try_get<TransformComponent>(childHandle);
            transform != nullptr) {
            const glm::mat4 local = glm::inverse(computeWorldMatrix(parentHandle)) * childWorld;
            glm::vec3 position;
            glm::vec3 scale;
            glm::quat orientation;
            TransformSystem::decompose(local, position, orientation, scale);
            transform->position = position;
            transform->scale = scale;
            transform->setOrientation(orientation);
        }
    }
    return true;
}

bool Scene::removeParent(Entity child, bool keepWorldTransform) {
    if (!isValid(child)) {
        return false;
    }
    const entt::entity handle = child.handle();
    const ParentComponent* parentComponent = m_registry.try_get<ParentComponent>(handle);
    if (parentComponent == nullptr || parentComponent->parent == entt::null) {
        return false;
    }
    glm::mat4 childWorld{1.0f};
    if (keepWorldTransform) {
        childWorld = computeWorldMatrix(handle);
    }
    unlinkFromParent(handle);
    m_registry.remove<ParentComponent>(handle);
    if (keepWorldTransform) {
        if (TransformComponent* transform = m_registry.try_get<TransformComponent>(handle);
            transform != nullptr) {
            glm::vec3 position;
            glm::vec3 scale;
            glm::quat orientation;
            TransformSystem::decompose(childWorld, position, orientation, scale);
            transform->position = position;
            transform->scale = scale;
            transform->setOrientation(orientation);
        }
    }
    return true;
}

Entity Scene::parentOf(Entity child) const {
    if (!isValid(child)) {
        return Entity{};
    }
    const ParentComponent* parentComponent = m_registry.try_get<ParentComponent>(child.handle());
    if (parentComponent == nullptr || parentComponent->parent == entt::null ||
        !m_registry.valid(parentComponent->parent)) {
        return Entity{};
    }
    return Entity{parentComponent->parent, const_cast<Scene*>(this)};
}

std::vector<Entity> Scene::childrenOf(Entity parent) const {
    std::vector<Entity> result;
    if (!isValid(parent)) {
        return result;
    }
    if (const ChildrenComponent* children = m_registry.try_get<ChildrenComponent>(parent.handle());
        children != nullptr) {
        result.reserve(children->children.size());
        for (const entt::entity childHandle : children->children) {
            if (m_registry.valid(childHandle)) {
                result.emplace_back(childHandle, const_cast<Scene*>(this));
            }
        }
    }
    return result;
}

bool Entity::isValid() const {
    return m_scene != nullptr && m_handle != entt::null && m_scene->registry().valid(m_handle);
}

std::string Entity::name() const {
    if (isValid() && hasComponent<TagComponent>()) {
        return m_scene->registry().get<TagComponent>(m_handle).tag;
    }
    return {};
}

bool Entity::setParent(Entity parent, bool keepWorldTransform) {
    return isValid() && m_scene->setParent(*this, parent, keepWorldTransform);
}

bool Entity::removeParent(bool keepWorldTransform) {
    return isValid() && m_scene->removeParent(*this, keepWorldTransform);
}

Entity Entity::parent() const {
    return isValid() ? m_scene->parentOf(*this) : Entity{};
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
    m_registry.storage<ParentComponent>().reserve(entityCount);
    m_registry.storage<ChildrenComponent>().reserve(entityCount);
    m_registry.storage<WorldTransformComponent>().reserve(entityCount);
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

#pragma once

#include "Core/Memory/PoolAllocator.h"
#include "Scene/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// Entity
//
// Lightweight handle to one entity in a Scene (an id plus a back-pointer).
// All component operations forward to the owning Scene's registry. Handles
// are cheap to copy and compare; they become invalid when the entity is
// destroyed, so check isValid() after destruction may have occurred.
// ---------------------------------------------------------------------------
class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene) {}

    template <typename T, typename... Args>
    T& addComponent(Args&&... args);
    template <typename T>
    T& getComponent();
    template <typename T>
    [[nodiscard]] bool hasComponent() const;
    template <typename T>
    void removeComponent();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] uint32_t id() const { return entt::to_integral(m_handle); }
    [[nodiscard]] entt::entity handle() const { return m_handle; }
    [[nodiscard]] std::string name() const;

    explicit operator bool() const { return isValid(); }
    bool operator==(const Entity& other) const { return m_handle == other.m_handle && m_scene == other.m_scene; }
    bool operator!=(const Entity& other) const { return !(*this == other); }

private:
    entt::entity m_handle = entt::null;
    Scene* m_scene = nullptr;
};

// ---------------------------------------------------------------------------
// Scene
//
// RAII owner of the ECS registry (the "world"). Entities and components
// live exactly as long as the Scene does; destroying the Scene destroys
// everything in it. Engine systems operate on a Scene through its
// registry, while game code should prefer the Entity handle API.
//
// The Scene knows nothing about windows or renderers — those live in
// systems that read the Scene.
// ---------------------------------------------------------------------------
class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    // Creates an entity with a TagComponent carrying the given name.
    Entity createEntity(const std::string& name = "Entity");
    // Destroys the entity and all of its components. No-op for stale handles.
    void destroyEntity(Entity entity);
    [[nodiscard]] bool isValid(Entity entity) const;
    [[nodiscard]] size_t entityCount() const { return m_entityCount; }

    // The first CameraComponent with isActive set, or nullptr. Shared by
    // the camera and render systems so "the active camera" is defined once.
    [[nodiscard]] CameraComponent* findActiveCamera();
    [[nodiscard]] const CameraComponent* findActiveCamera() const;

    // Pre-allocates component storage for the given number of entities
    // (entities, transforms, meshes, cameras, spins) so scene building
    // and hot-path iteration don't pay heap growth costs. Call before
    // creating entities; harmless to call again later.
    void reserveCapacity(size_t entityCount);

    // Memory pool backing all component storage of this scene.
    [[nodiscard]] memory::PoolStats memoryStats() const { return m_registry.get_allocator().stats(); }

    // The registry type: EnTT backed by the engine's paginated pool
    // allocator — every storage draws blocks from one shared pool
    // instead of fragmenting the global heap.
    using Registry = entt::basic_registry<entt::entity, memory::PoolAllocator<entt::entity>>;

    // Direct registry access for engine systems. Prefer Entity handles in
    // gameplay-facing code; systems legitimately need views/queries.
    [[nodiscard]] Registry& registry() { return m_registry; }
    [[nodiscard]] const Registry& registry() const { return m_registry; }

private:
    Registry m_registry;
    size_t m_entityCount = 0; // Scene-created, still-alive entities
};

// --- Entity template implementations (need the complete Scene) -------------

template <typename T, typename... Args>
T& Entity::addComponent(Args&&... args) {
    return m_scene->registry().emplace<T>(m_handle, std::forward<Args>(args)...);
}

template <typename T>
T& Entity::getComponent() {
    return m_scene->registry().get<T>(m_handle);
}

template <typename T>
bool Entity::hasComponent() const {
    return m_scene != nullptr && m_scene->registry().all_of<T>(m_handle);
}

template <typename T>
void Entity::removeComponent() {
    m_scene->registry().remove<T>(m_handle);
}

inline bool Entity::isValid() const {
    return m_scene != nullptr && m_handle != entt::null && m_scene->registry().valid(m_handle);
}

inline std::string Entity::name() const {
    if (isValid() && hasComponent<TagComponent>()) {
        return m_scene->registry().get<TagComponent>(m_handle).tag;
    }
    return {};
}

} // namespace engine

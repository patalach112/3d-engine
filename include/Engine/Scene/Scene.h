#pragma once
#include "Core/Export.h"

#include "Assets/AssetManager.h"
#include "Core/EventBus.h"
#include "Core/Memory/PoolAllocator.h"
#include "Scene/Components.h"
#include "Scene/DynamicAABBTree.h"
#include "Scene/Raycast.h"
#include "Scene/SceneEvents.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

class Frustum;
class Scene;

// ---------------------------------------------------------------------------
// Entity
//
// Lightweight handle to one entity in a Scene (an id plus a back-pointer).
// All component operations forward to the owning Scene's registry. Handles
// are cheap to copy and compare; they become invalid when the entity is
// destroyed, so check isValid() after destruction may have occurred.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API Entity {
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

    // Scene-graph hierarchy (Phase 18): forwards to Scene::setParent /
    // removeParent. Returns false when the link was rejected.
    bool setParent(Entity parent, bool keepWorldTransform = false);
    bool removeParent(bool keepWorldTransform = false);
    [[nodiscard]] Entity parent() const;

    explicit operator bool() const { return isValid(); }
    bool operator==(const Entity& other) const { return m_handle == other.m_handle && m_scene == other.m_scene; }
    bool operator!=(const Entity& other) const { return !(*this == other); }

private:
    entt::entity m_handle = entt::null;
    Scene* m_scene = nullptr;
};

// What Scene::destroyEntity does with an entity's children
// (Phase 18): Orphan detaches them to the root, recomputing their
// local transforms so their world placement does not change;
// Cascade destroys the whole subtree (each descendant publishes its
// own EntityDestroyedEvent first).
enum class HierarchyDestroyMode { Orphan, Cascade };

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
class ENGINE_SCENE_API Scene {
public:
    // The constructor primes the EnTT storage of every built-in
    // component type from engine code. This is a module-boundary
    // contract (Phase 25): game modules are separate binaries that
    // are unloaded while scenes outlive them (hot reload), and a
    // component storage carries the vtable and shared_ptr control
    // block of whichever binary's template instantiation created
    // it — a storage first created by module code would later be
    // destroyed through unmapped code. With the storages primed,
    // module code only ever finds existing, host-owned storages.
    // Corollary: module-local component types must never be stored
    // in a host Scene; module state belongs in script instances
    // (INativeScript) that the module destroys in onUnload.
    Scene();
    ~Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    // Creates an entity with a TagComponent carrying the given name.
    Entity createEntity(const std::string& name = "Entity");
    // Spawns a fully-wired entity for an imported/registered mesh:
    //
    //   TransformComponent  scale = userScale * mesh.bounds.autoScaleFactor
    //                       (the mesh's longest dimension becomes
    //                       userScale's longest dimension, whatever
    //                       units the source file used)
    //   MeshComponent       asset name + the mesh's raw local AABB
    //   RigidBodyComponent  dynamic body of the given mass
    //   ColliderComponent   box with halfExtents = bounds.extents and
    //                       centerOffset = bounds.center
    //
    // Mesh, collider and transform all consume the same raw bounds, so
    // the physics hitbox is a 1:1 match of the rendered bounds at any
    // user scale with no manual collider configuration. Returns an
    // invalid Entity when the asset carries no geometry.
    Entity spawnImportedModel(const MeshAsset& asset, const std::string& name, const glm::vec3& position,
                              const glm::vec3& userScale = glm::vec3{1.0f}, float mass = 1.0f);
    Entity spawnImportedModel(const MeshAsset& asset, const std::string& name, const glm::vec3& position,
                              float userScale, float mass = 1.0f);
    // Destroys the entity and all of its components. No-op for stale handles.
    void destroyEntity(Entity entity);
    [[nodiscard]] bool isValid(Entity entity) const;
    [[nodiscard]] size_t entityCount() const { return m_entityCount; }

    // The first CameraComponent with isActive set, or nullptr. Shared by
    // the camera and render systems so "the active camera" is defined once.
    [[nodiscard]] CameraComponent* findActiveCamera();
    [[nodiscard]] const CameraComponent* findActiveCamera() const;
    // Entity carrying the active camera (entt::null when there is none).
    // Systems that need the camera's sibling Transform/RigidBody/Collider
    // components (physical players) use this instead of duplicating the
    // active-camera search.
    [[nodiscard]] entt::entity findActiveCameraEntity() const;

    // --- Raycasting / mouse picking ------------------------------------------
    //
    // All three queries walk the entities that carry a TransformComponent
    // and test, per entity, the first applicable representation:
    //
    //   1. (raycastPrecise only) the real mesh triangles, when the
    //      entity has a MeshComponent whose asset id the supplied
    //      MeshResolver can resolve — the mesh surface is then the
    //      exact truth and bounds/colliders of that entity are ignored;
    //   2. an exact box test for Box/Obb ColliderComponents, performed
    //      in collider local space so rotated and non-uniformly scaled
    //      boxes are hit precisely (this is the volume physics uses);
    //   3. the world-space AABB of a visible MeshComponent's local
    //      bounds (MeshBounds data, via MeshComponent::localBounds).
    //
    // Other collider shapes (sphere, capsule, plane, convex hull) are
    // not raycastable yet; such entities are still found through their
    // mesh when they have one. Hits behind the origin or farther than
    // maxDistance are rejected. The ray's direction must be normalized.
    using MeshResolver = std::function<const MeshAsset*(std::string_view assetId)>;

    // Closest hit along the ray, if any. `layerMask` optionally
    // restricts hits to entities whose collision layer (their
    // ColliderComponent's layer, CollisionLayer::Default for
    // collider-less entities) intersects it — the same layer values
    // physics filtering uses (Phase 15).
    [[nodiscard]] bool raycast(const Ray& ray, RaycastHit& outHit,
                               float maxDistance = std::numeric_limits<float>::max(),
                               uint32_t layerMask = CollisionLayer::All) const;
    // Every hit along the ray, sorted by ascending distance. Clears
    // outHits first; returns the number of hits.
    [[nodiscard]] size_t raycastAll(const Ray& ray, std::vector<RaycastHit>& outHits,
                                    float maxDistance = std::numeric_limits<float>::max(),
                                    uint32_t layerMask = CollisionLayer::All) const;
    // Closest hit, testing real mesh triangles wherever resolveMesh
    // resolves a MeshComponent's asset id (entities whose asset does
    // not resolve fall back to the bounds/collider tests above).
    [[nodiscard]] bool raycastPrecise(const Ray& ray, RaycastHit& outHit, const MeshResolver& resolveMesh,
                                      float maxDistance = std::numeric_limits<float>::max(),
                                      uint32_t layerMask = CollisionLayer::All) const;

    // Entities whose world bounds (the same BVH the raycasts use)
    // intersect the frustum — a conservative superset of the visible
    // set, gathered in one pruned tree traversal (Phase 21).
    [[nodiscard]] std::vector<entt::entity> queryFrustum(const Frustum& frustum) const;

    // Pre-allocates component storage for the given number of entities
    // (entities, transforms, meshes, cameras, spins) so scene building
    // and hot-path iteration don't pay heap growth costs. Call before
    // creating entities; harmless to call again later.
    void reserveCapacity(size_t entityCount);

    // Event output (Phase 17): when a bus is attached, destroyEntity
    // publishes EntityDestroyedEvent just before the entity is
    // destroyed (components still readable inside listeners).
    void setEventBus(EventBus* bus) { m_eventBus = bus; }

    // --- Scene-graph hierarchy (Phase 18) --------------------------------
    // Parent/Children components are maintained exclusively through
    // these methods; patching them directly can desynchronize the
    // two sides of a link. Transforms are LOCAL to the parent and
    // are only composed into world placement by TransformSystem.
    //
    // Links that would create a cycle (including self-parenting) or
    // involve invalid entities are rejected (false, nothing changes).
    // keepWorldTransform recomputes the child's local transform so
    // its world placement survives the (re)parenting; otherwise the
    // authored local transform is kept as-is (the common case).
    bool setParent(Entity child, Entity parent, bool keepWorldTransform = false);
    // Detaches the child to the root level. Returns false when the
    // entity was already a root (or is invalid).
    bool removeParent(Entity child, bool keepWorldTransform = false);
    [[nodiscard]] Entity parentOf(Entity child) const;
    [[nodiscard]] std::vector<Entity> childrenOf(Entity parent) const;
    [[nodiscard]] bool isDescendantOf(Entity entity, Entity ancestor) const;

    // Destruction policy for children of destroyed entities
    // (default: Orphan — children survive at the root with their
    // world placement preserved).
    void setHierarchyDestroyMode(HierarchyDestroyMode mode) { m_hierarchyDestroyMode = mode; }
    [[nodiscard]] HierarchyDestroyMode hierarchyDestroyMode() const { return m_hierarchyDestroyMode; }

    // --- Scene serialization (Phase 19) --------------------------------
    // The whole scene as a JSON document: every entity with its
    // components (tag, transform, camera, mesh, spin, rigid body,
    // collider, gravity field, joint, thruster), the hierarchy
    // (stored as parent indices, rebuilt through setParent) and
    // joint links (entity indices, remapped to the fresh handles).
    // ChildrenComponent and WorldTransformComponent are derived
    // state and are reconstructed, never stored. Floats print in
    // shortest round-trip form, so values survive bit-for-bit.
    //
    // loadFromString/loadFromFile validate the document first and
    // leave the scene untouched on malformed input; on success the
    // current registry is cleared wholesale (silently — no
    // EntityDestroyedEvents) and rebuilt. PhysicsSystem spawn
    // snapshots belong to the old entities: re-capture after a load
    // before resetting.
    [[nodiscard]] std::string saveToString() const;
    bool loadFromString(std::string_view text);
    bool saveToFile(const std::string& filepath) const;
    bool loadFromFile(const std::string& filepath);

    // --- Binary scene serialization (Phase 30, .gbq) -------------------
    // The same scene in the custom GBQ1 container: magic header,
    // a table-of-contents section index for O(1) section access,
    // and fixed-layout binary component records (bit-exact floats,
    // no text parsing). Covers everything the JSON format does
    // plus point/spot lights and native script names. The free
    // serializeScene()/deserializeScene() wrappers live in
    // Scene/SceneBinarySerialization.h. loadFromGbqFile validates
    // the whole file before touching the scene (left untouched on
    // malformed input); on success it clears and rebuilds like
    // loadFromString.
    bool saveToGbqFile(const std::string& filepath) const;
    bool loadFromGbqFile(const std::string& filepath);

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
    // Broadphase acceleration for the raycast queries above: a dynamic
    // AABB tree over every pickable entity's world bounds (collider
    // AABB unioned with the visible mesh's world AABB — the same
    // candidacy the exact tests use). Physics writes transforms
    // directly, so no EnTT hooks can keep it current; instead the
    // tree is synced at the top of every raycast query (fat boxes
    // absorb small motions, so syncing is mostly a bounds pass) and
    // the BVH then prunes the exact tests to the ray's actual path.
    struct RaycastProxyEntry {
        DynamicAABBTree::ProxyId proxy = DynamicAABBTree::kNullProxy;
        glm::vec3 lastCenter{0.0f}; // movement since here predicts the fat box
    };
    void syncRaycastTree() const;
    // BVH traversal + exact per-entity tests. With closestOnly, each
    // hit found tightens the traversal's clip distance, pruning
    // subtrees that cannot contain a closer hit.
    void collectBVHHits(const Ray& ray, float maxDistance, const MeshResolver* resolver, bool closestOnly,
                        uint32_t layerMask, std::vector<RaycastHit>& outHits) const;

    Registry m_registry;
    size_t m_entityCount = 0; // Scene-created, still-alive entities
    EventBus* m_eventBus = nullptr;
    HierarchyDestroyMode m_hierarchyDestroyMode = HierarchyDestroyMode::Orphan;

    // World matrix of one entity computed on demand by walking its
    // ancestor chain (root-first composition of local model
    // matrices; transform-less ancestors contribute identity). Used
    // by keepWorldTransform reparenting and orphan preservation,
    // where the answer must not depend on TransformSystem having
    // run. Cycle-safe: the walk is depth-capped.
    [[nodiscard]] glm::mat4 computeWorldMatrix(entt::entity entity) const;
    // Removes `child` from its current parent's ChildrenComponent
    // (if any) without touching the child's ParentComponent.
    void unlinkFromParent(entt::entity child);
    mutable DynamicAABBTree m_raycastTree;
    mutable std::unordered_map<uint32_t /*entt integral*/, RaycastProxyEntry> m_raycastProxies;
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

// Entity::isValid() / Entity::name() are defined in Scene.cpp, not
// inline here: Entity is an exported class, and out-of-line inline
// definitions of its members clash with dllimport on Windows.

} // namespace engine

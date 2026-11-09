#include "Scene/Raycast.h"

#include "Scene/Scene.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>

namespace engine {

Ray cameraRay(const Camera& camera, glm::vec2 ndc, float aspectRatio) {
    const glm::mat4 viewProjection = camera.projectionMatrix(aspectRatio) * camera.viewMatrix();
    Ray ray = rayThroughNdc(ndc, glm::inverse(viewProjection));
    // rayThroughNdc's origin sits on the near plane; picking distances
    // are measured from the eye, so snap the origin to the camera.
    ray.origin = camera.position();
    return ray;
}

namespace {

// One entity's nearest intersection along the ray, in world space.
struct Candidate {
    float distance = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

// Transforms a world ray into an entity's local (model) space. The
// direction is intentionally NOT re-normalized: an affine transform
// preserves the ray parameter, so the t values produced by local
// intersection tests are world distances along the original ray
// (whose direction is unit length).
Ray toLocalRay(const Ray& ray, const glm::mat4& inverseModel) {
    Ray local;
    local.origin = glm::vec3{inverseModel * glm::vec4{ray.origin, 1.0f}};
    local.direction = glm::vec3{inverseModel * glm::vec4{ray.direction, 0.0f}};
    return local;
}

// Local surface normal -> world surface normal: normals transform by
// the inverse-transpose, and the result is oriented against the ray
// so a hit always reports the face the ray arrived from (relevant
// for mirrored scales and two-sided triangles).
glm::vec3 toWorldNormal(const glm::mat4& inverseModel, const glm::vec3& localNormal, const Ray& ray) {
    glm::vec3 normal = glm::mat3{glm::transpose(inverseModel)} * localNormal;
    const float lengthSq = glm::dot(normal, normal);
    normal = lengthSq > 1e-20f ? normal / std::sqrt(lengthSq) : -ray.direction;
    if (glm::dot(normal, ray.direction) > 0.0f) {
        normal = -normal;
    }
    return normal;
}

// Exact per-triangle test against a mesh asset, in the mesh's local
// space, with the asset's own bounds as a broadphase rejection.
bool testMeshTriangles(const MeshAsset& asset, const glm::mat4& model, const Ray& ray, float maxDistance,
                       Candidate& out) {
    if (asset.vertices.size() < 3) {
        return false;
    }
    const glm::mat4 inverseModel = glm::inverse(model);
    const Ray localRay = toLocalRay(ray, inverseModel);
    float boundsDistance = 0.0f;
    glm::vec3 boundsNormal{0.0f};
    if (!rayIntersectsAABB(localRay, asset.bounds.aabb(), boundsDistance, boundsNormal) ||
        boundsDistance > maxDistance) {
        return false;
    }
    // Indexed meshes walk their index buffer; an index-less asset is
    // treated as a triangle soup over consecutive vertices.
    const bool indexed = !asset.indices.empty();
    const size_t indexCount = indexed ? asset.indices.size() : asset.vertices.size();
    float bestDistance = maxDistance;
    glm::vec3 bestNormal{0.0f, 1.0f, 0.0f};
    bool found = false;
    for (size_t i = 0; i + 2 < indexCount; i += 3) {
        const glm::vec3& a = asset.vertices[indexed ? asset.indices[i] : i].position;
        const glm::vec3& b = asset.vertices[indexed ? asset.indices[i + 1] : i + 1].position;
        const glm::vec3& c = asset.vertices[indexed ? asset.indices[i + 2] : i + 2].position;
        float distance = 0.0f;
        glm::vec2 barycentric{0.0f};
        if (rayIntersectsTriangle(localRay, a, b, c, distance, barycentric) && distance < bestDistance) {
            bestDistance = distance;
            bestNormal = glm::cross(b - a, c - a);
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    out.distance = bestDistance;
    out.position = ray.at(bestDistance);
    out.normal = toWorldNormal(inverseModel, bestNormal, ray);
    return true;
}

// Exact oriented-box test for box/OBB colliders, done in collider
// local space (centerOffset +- halfExtents, before transform scale is
// applied — the model matrix consumes both).
bool testBoxCollider(const ColliderComponent& collider, const glm::mat4& model, const Ray& ray, float maxDistance,
                     Candidate& out) {
    const AABB localBox{collider.centerOffset - collider.halfExtents, collider.centerOffset + collider.halfExtents};
    const glm::mat4 inverseModel = glm::inverse(model);
    const Ray localRay = toLocalRay(ray, inverseModel);
    float distance = 0.0f;
    glm::vec3 localNormal{0.0f};
    if (!rayIntersectsAABB(localRay, localBox, distance, localNormal) || distance > maxDistance) {
        return false;
    }
    out.distance = distance;
    out.position = ray.at(distance);
    out.normal = toWorldNormal(inverseModel, localNormal, ray);
    return true;
}

// World-AABB test over a mesh's local bounds (the bounds-level mesh
// test: exact for unrotated entities, the tight world box otherwise).
bool testMeshBounds(const MeshComponent& mesh, const glm::mat4& model, const Ray& ray, float maxDistance,
                    Candidate& out) {
    const AABB worldBox = mesh.localBounds.transformed(model);
    float distance = 0.0f;
    glm::vec3 normal{0.0f};
    if (!rayIntersectsAABB(ray, worldBox, distance, normal) || distance > maxDistance) {
        return false;
    }
    out.distance = distance;
    out.position = ray.at(distance);
    out.normal = normal;
    return true;
}

// Resolves one entity to at most one candidate hit — the first
// applicable representation wins:
//   1. precise triangle test, when a resolver is supplied and the
//      entity's mesh asset resolves (the mesh surface is the truth);
//   2. an exact box/OBB collider test (physics-true occupied volume);
//   3. the mesh's world bounds (visible meshes only).
// Other collider shapes (sphere, capsule, plane, hull) are not
// raycastable yet and fall through to the mesh test when present.
bool testEntity(const TransformComponent& transform, const ColliderComponent* collider, const MeshComponent* mesh,
                const Scene::MeshResolver* resolver, const Ray& ray, float maxDistance, Candidate& out) {
    const glm::mat4 model = transform.modelMatrix();
    if (resolver != nullptr && mesh != nullptr && mesh->visible) {
        if (const MeshAsset* asset = (*resolver)(mesh->assetId); asset != nullptr) {
            return testMeshTriangles(*asset, model, ray, maxDistance, out);
        }
    }
    if (collider != nullptr &&
        (collider->type == ColliderComponent::Type::Box || collider->type == ColliderComponent::Type::Obb)) {
        return testBoxCollider(*collider, model, ray, maxDistance, out);
    }
    if (mesh != nullptr && mesh->visible) {
        return testMeshBounds(*mesh, model, ray, maxDistance, out);
    }
    return false;
}

void collectHits(const Scene& scene, const Ray& ray, float maxDistance, const Scene::MeshResolver* resolver,
                 std::vector<RaycastHit>& outHits) {
    const auto& registry = scene.registry();
    const auto view = registry.view<TransformComponent>();
    Candidate candidate;
    for (const entt::entity entity : view) {
        const TransformComponent& transform = view.get<TransformComponent>(entity);
        const ColliderComponent* collider = registry.try_get<ColliderComponent>(entity);
        const MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        if (testEntity(transform, collider, mesh, resolver, ray, maxDistance, candidate)) {
            outHits.push_back(RaycastHit{entity, candidate.distance, candidate.position, candidate.normal});
        }
    }
}

} // namespace

bool Scene::raycast(const Ray& ray, RaycastHit& outHit, float maxDistance) const {
    std::vector<RaycastHit> hits;
    collectHits(*this, ray, maxDistance, nullptr, hits);
    if (hits.empty()) {
        return false;
    }
    outHit = *std::min_element(hits.begin(), hits.end(),
                               [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });
    return true;
}

size_t Scene::raycastAll(const Ray& ray, std::vector<RaycastHit>& outHits, float maxDistance) const {
    outHits.clear();
    collectHits(*this, ray, maxDistance, nullptr, outHits);
    std::sort(outHits.begin(), outHits.end(),
              [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });
    return outHits.size();
}

bool Scene::raycastPrecise(const Ray& ray, RaycastHit& outHit, const MeshResolver& resolveMesh,
                           float maxDistance) const {
    std::vector<RaycastHit> hits;
    collectHits(*this, ray, maxDistance, &resolveMesh, hits);
    if (hits.empty()) {
        return false;
    }
    outHit = *std::min_element(hits.begin(), hits.end(),
                               [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });
    return true;
}

} // namespace engine

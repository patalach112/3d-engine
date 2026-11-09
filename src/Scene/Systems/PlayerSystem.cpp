#include "Scene/Systems/PlayerSystem.h"

#include "Platform/Input.h"
#include "Scene/Scene.h"

#include <glm/geometric.hpp>

namespace engine {
namespace {

struct PlayerParts {
    CameraComponent* camera = nullptr;
    TransformComponent* transform = nullptr;
    RigidBodyComponent* body = nullptr;
    ColliderComponent* collider = nullptr;
};

PlayerParts findPlayer(Scene& scene) {
    PlayerParts parts;
    const entt::entity entity = scene.findActiveCameraEntity();
    if (entity == entt::null) {
        return parts;
    }
    auto& registry = scene.registry();
    if (!registry.all_of<CameraComponent, TransformComponent, RigidBodyComponent, ColliderComponent>(entity)) {
        return parts;
    }
    parts.camera = &registry.get<CameraComponent>(entity);
    parts.transform = &registry.get<TransformComponent>(entity);
    parts.body = &registry.get<RigidBodyComponent>(entity);
    parts.collider = &registry.get<ColliderComponent>(entity);
    return parts;
}

} // namespace

bool PlayerSystem::hasPhysicalPlayer(const Scene& scene) {
    const entt::entity entity = scene.findActiveCameraEntity();
    return entity != entt::null &&
           scene.registry().all_of<TransformComponent, RigidBodyComponent, ColliderComponent>(entity);
}

bool PlayerSystem::driveMovement(Scene& scene, glm::vec3 localDirection, double fixedDeltaSeconds, bool boost) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.body == nullptr || fixedDeltaSeconds <= 0.0) {
        return false;
    }

    Camera& camera = player.camera->camera;
    glm::vec3 forward = camera.front();
    forward.y = 0.0f;
    forward = glm::dot(forward, forward) > 1e-8f ? glm::normalize(forward) : glm::vec3{0.0f, 0.0f, -1.0f};
    glm::vec3 right = camera.right();
    right.y = 0.0f;
    right = glm::dot(right, right) > 1e-8f ? glm::normalize(right) : glm::vec3{1.0f, 0.0f, 0.0f};

    glm::vec3 wish = right * localDirection.x + forward * localDirection.z;
    if (glm::dot(wish, wish) > 1.0f) {
        wish = glm::normalize(wish);
    }
    const glm::vec3 desired = wish * camera.movementSpeed(boost);
    const glm::vec3 smoothed = camera.smoothedMovementVelocity(glm::vec3{desired.x, 0.0f, desired.z},
                                                               fixedDeltaSeconds);
    player.body->velocity.x = smoothed.x;
    player.body->velocity.z = smoothed.z;
    if (localDirection.y != 0.0f) {
        // Vertical thrust (jump/fly keys) overrides gravity for this step;
        // with no vertical input the physics step's gravity owns Y.
        player.body->velocity.y = localDirection.y * camera.movementSpeed(boost);
    }
    return true;
}

bool PlayerSystem::update(Scene& scene, const Input& input, double fixedDeltaSeconds) {
    glm::vec3 localDirection{0.0f};
    if (input.isKeyDown(Key::W)) {
        localDirection.z += 1.0f;
    }
    if (input.isKeyDown(Key::S)) {
        localDirection.z -= 1.0f;
    }
    if (input.isKeyDown(Key::D)) {
        localDirection.x += 1.0f;
    }
    if (input.isKeyDown(Key::A)) {
        localDirection.x -= 1.0f;
    }
    if (input.isKeyDown(Key::Space) || input.isKeyDown(Key::E)) {
        localDirection.y += 1.0f;
    }
    if (input.isKeyDown(Key::Q)) {
        localDirection.y -= 1.0f;
    }
    const bool boost = input.isKeyDown(Key::LeftShift) || input.isKeyDown(Key::RightShift);
    return driveMovement(scene, localDirection, fixedDeltaSeconds, boost);
}

bool PlayerSystem::syncCamera(Scene& scene, float eyeHeight) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.transform == nullptr) {
        return false;
    }
    player.camera->camera.setPosition(player.transform->position + glm::vec3{0.0f, eyeHeight, 0.0f});
    return true;
}

} // namespace engine

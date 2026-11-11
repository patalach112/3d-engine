#include "Scene/Systems/PlayerSystem.h"

#include "Platform/Input.h"
#include "Scene/Scene.h"

#include <cmath>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

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

// The character-driven player: the physical player set plus a
// CharacterControllerComponent binding it to the movement core.
struct CharacterParts {
    PlayerParts player;
    CharacterControllerComponent* controller = nullptr;
};

CharacterParts findCharacterPlayer(Scene& scene) {
    CharacterParts parts;
    parts.player = findPlayer(scene);
    if (parts.player.camera == nullptr) {
        return parts;
    }
    const entt::entity entity = scene.findActiveCameraEntity();
    auto& registry = scene.registry();
    if (registry.all_of<CharacterControllerComponent>(entity)) {
        parts.controller = &registry.get<CharacterControllerComponent>(entity);
    }
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
    // Character-driven players tick the decoupled MovementSystem;
    // everyone else keeps the legacy smoothed-velocity path.
    if (findCharacterPlayer(scene).controller != nullptr) {
        return updateCharacter(scene, input, fixedDeltaSeconds);
    }
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

bool PlayerSystem::ensureCharacterController(Scene& scene, float groundY) {
    const PlayerParts player = findPlayer(scene);
    if (player.camera == nullptr || player.transform == nullptr) {
        return false;
    }
    const entt::entity entity = scene.findActiveCameraEntity();
    auto& registry = scene.registry();
    if (registry.all_of<CharacterControllerComponent>(entity)) {
        return true;
    }
    CharacterControllerComponent controller;
    controller.settings.groundY = groundY;
    controller.state.position = player.transform->position;
    controller.state.grounded = true;
    registry.emplace<CharacterControllerComponent>(entity, controller);
    return true;
}

bool PlayerSystem::driveCharacter(Scene& scene, const InputCmd& cmd, double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return false;
    }
    const CharacterParts parts = findCharacterPlayer(scene);
    if (parts.controller == nullptr || parts.player.body == nullptr || !parts.controller->enabled) {
        return false;
    }
    CharacterControllerComponent& controller = *parts.controller;
    RigidBodyComponent& body = *parts.player.body;

    // Ground/collision handshake, part 1: PhysicsSystem resolved
    // the capsule last step — its transform is the positional
    // truth the movement state resumes from.
    controller.state.position = parts.player.transform->position;

    // The shared movement core, ticked directly with the caller's
    // real dt. The component owns the local cmd_number sequence.
    InputCmd sequenced = cmd;
    sequenced.cmdNumber = ++controller.cmdCounter;
    playerMove(sequenced, controller.state, controller.settings, static_cast<float>(deltaSeconds));

    // Handshake, part 2: the controller owns velocity (and with it
    // gravity and sleep — PhysicsSystem must not double-integrate
    // either); the solver integrates the capsule from here and
    // resolves floor/wall/ceiling collisions.
    body.velocity = controller.state.velocity;
    body.useGravity = false;
    body.allowSleep = false;
    body.sleeping = false;
    body.sleepTimer = 0.0f;
    return true;
}

bool PlayerSystem::updateCharacter(Scene& scene, const Input& input, double deltaSeconds) {
    const CharacterParts parts = findCharacterPlayer(scene);
    if (parts.controller == nullptr || parts.player.camera == nullptr) {
        return false;
    }
    InputCmd cmd;
    if (input.isKeyDown(Key::W)) {
        cmd.forward += 1.0f;
    }
    if (input.isKeyDown(Key::S)) {
        cmd.forward -= 1.0f;
    }
    if (input.isKeyDown(Key::D)) {
        cmd.strafe += 1.0f;
    }
    if (input.isKeyDown(Key::A)) {
        cmd.strafe -= 1.0f;
    }
    cmd.jump = input.isKeyDown(Key::Space);
    // Movement yaw convention (0 = -Z) from the camera's facing.
    const glm::vec3 front = parts.player.camera->camera.front();
    cmd.yawDegrees = glm::degrees(std::atan2(-front.x, -front.z));
    return driveCharacter(scene, cmd, deltaSeconds);
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

#include "Scene/Systems/CameraSystem.h"

#include "Platform/Input.h"
#include "Scene/Scene.h"
#include "Scene/Systems/PlayerSystem.h"

namespace engine {

void CameraSystem::update(Scene& scene, const Input& input, double deltaSeconds) {
    CameraComponent* activeCamera = scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return;
    }

    if (input.isMouseCaptured()) {
        activeCamera->camera.processLook(input.mouseDelta());
    }
    // Advance the accumulated look target with this frame's real delta.
    activeCamera->camera.update(deltaSeconds);

    // Physical players are moved by PlayerSystem + PhysicsSystem in the
    // fixed update; the camera only follows the simulated capsule. Calling
    // processMovement() here would teleport the eye through colliders.
    if (PlayerSystem::syncCamera(scene)) {
        return;
    }
    activeCamera->camera.processMovement(input, deltaSeconds);
}

} // namespace engine

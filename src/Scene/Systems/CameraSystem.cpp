#include "Scene/Systems/CameraSystem.h"

#include "Platform/Input.h"
#include "Scene/Scene.h"

namespace engine {

void CameraSystem::update(Scene& scene, const Input& input, double deltaSeconds) {
    CameraComponent* activeCamera = scene.findActiveCamera();
    if (activeCamera == nullptr) {
        return;
    }

    if (input.isMouseCaptured()) {
        activeCamera->camera.processLook(input.mouseDelta());
    }
    // Advance the accumulated look target with this frame's real delta,
    // then integrate smoothed movement velocity over the same interval.
    activeCamera->camera.update(deltaSeconds);
    activeCamera->camera.processMovement(input, deltaSeconds);
}

} // namespace engine

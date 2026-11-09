#include "Scene/Systems/RotationSystem.h"

#include "Scene/Scene.h"

#include <glm/common.hpp>

namespace engine {

void RotationSystem::update(Scene& scene, double fixedDeltaSeconds) {
    const float delta = static_cast<float>(fixedDeltaSeconds);
    scene.registry().view<TransformComponent, SpinComponent>().each(
        [delta](TransformComponent& transform, const SpinComponent& spin) {
            transform.rotationDegrees =
                glm::mod(transform.rotationDegrees + spin.degreesPerSecond * delta, 360.0f);
        });
}

} // namespace engine

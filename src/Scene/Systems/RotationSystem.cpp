#include "Scene/Systems/RotationSystem.h"

#include "Core/Jobs/JobSystem.h"
#include "Scene/Scene.h"

#include <glm/common.hpp>

#include <vector>

namespace engine {
namespace {

struct SpinWork {
    TransformComponent* transform;
    glm::vec3 deltaDegrees; // spin * fixedDelta, precomputed on the main thread
};

void applySpin(const SpinWork& work) {
    work.transform->rotationDegrees = glm::mod(work.transform->rotationDegrees + work.deltaDegrees, 360.0f);
}

std::vector<SpinWork> collectWork(Scene& scene, float fixedDelta) {
    std::vector<SpinWork> work;
    scene.registry().view<TransformComponent, SpinComponent>().each(
        [&work, fixedDelta](TransformComponent& transform, const SpinComponent& spin) {
            work.push_back(SpinWork{&transform, spin.degreesPerSecond * fixedDelta});
        });
    return work;
}

} // namespace

void RotationSystem::update(Scene& scene, double fixedDeltaSeconds) {
    const std::vector<SpinWork> work = collectWork(scene, static_cast<float>(fixedDeltaSeconds));
    for (const SpinWork& item : work) {
        applySpin(item);
    }
}

void RotationSystem::update(Scene& scene, double fixedDeltaSeconds, JobSystem& jobs) {
    const std::vector<SpinWork> work = collectWork(scene, static_cast<float>(fixedDeltaSeconds));
    // Synchronous parallel loop: returning from parallelFor is the barrier
    // that makes the subsequent variable update/render safe.
    jobs.parallelFor(work.size(), 0, [&work](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            applySpin(work[i]);
        }
    });
}

} // namespace engine

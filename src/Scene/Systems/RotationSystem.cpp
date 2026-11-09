#include "Scene/Systems/RotationSystem.h"

#include "Core/Jobs/JobSystem.h"
#include "Scene/DenseIteration.h"
#include "Scene/Scene.h"

#include <glm/common.hpp>

#include <type_traits>
#include <utility>

namespace engine {
namespace {

using TransformStorage =
    std::remove_reference_t<decltype(std::declval<Scene::Registry&>().storage<TransformComponent>())>;
using SpinStorage =
    std::remove_reference_t<decltype(std::declval<Scene::Registry&>().storage<SpinComponent>())>;

// Dense ranges for all entities that have both Transform and Spin.
//
// Uses an EnTT *owning group*: the registry physically rearranges both
// component storages so grouped components occupy the mutually
// index-aligned dense-index prefix [0, group.size()) of each storage.
// Iteration is then dense-index arithmetic over two packed arrays — no
// sparse-set lookups per entity, no pointer snapshots to chase.
struct AlignedRanges {
    TransformStorage* transforms = nullptr;
    SpinStorage* spins = nullptr;
    size_t count = 0;
};

AlignedRanges alignedRanges(Scene& scene) {
    auto group = scene.registry().group<TransformComponent, SpinComponent>();
    const size_t count = group.size();
    if (count == 0) {
        return {};
    }
    auto& transforms = *group.storage<TransformComponent>();
    auto& spins = *group.storage<SpinComponent>();
    return {&transforms, &spins, count};
}

void applyRange(const AlignedRanges& ranges, float fixedDelta, size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
        TransformComponent& transform = *denseElementAt(*ranges.transforms, i);
        const SpinComponent& spin = *denseElementAt(*ranges.spins, i);
        transform.rotationDegrees = glm::mod(transform.rotationDegrees + spin.degreesPerSecond * fixedDelta, 360.0f);
    }
}

} // namespace

void RotationSystem::update(Scene& scene, double fixedDeltaSeconds) {
    const AlignedRanges ranges = alignedRanges(scene);
    applyRange(ranges, static_cast<float>(fixedDeltaSeconds), 0, ranges.count);
}

void RotationSystem::update(Scene& scene, double fixedDeltaSeconds, JobSystem& jobs) {
    const AlignedRanges ranges = alignedRanges(scene);
    const float fixedDelta = static_cast<float>(fixedDeltaSeconds);
    // Disjoint element writes across two dense arrays (TransformComponents
    // are cache-line padded, so workers never false-share); returning from
    // parallelFor is the barrier before any other system touches them.
    jobs.parallelFor(ranges.count, 0, [&ranges, fixedDelta](size_t begin, size_t end) {
        applyRange(ranges, fixedDelta, begin, end);
    });
}

} // namespace engine

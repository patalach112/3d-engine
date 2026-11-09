#pragma once
#include "Core/Export.h"

namespace engine {

class JobSystem;
class Scene;

// ---------------------------------------------------------------------------
// RotationSystem
//
// Integrates SpinComponent into TransformComponent rotation. Runs in the
// loop's fixed update, so spinning is deterministic and independent of
// frame rate.
//
// Iteration runs over an EnTT owning group, so Transform and Spin
// components form two contiguous, index-aligned dense arrays; the
// JobSystem overload then writes to disjoint array elements concurrently
// (TransformComponent's cache-line padding prevents false sharing). No
// structural registry changes happen while jobs run, and parallelFor()'s
// return is the barrier before the next system.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API RotationSystem {
public:
    static void update(Scene& scene, double fixedDeltaSeconds);
    static void update(Scene& scene, double fixedDeltaSeconds, JobSystem& jobs);
};

} // namespace engine

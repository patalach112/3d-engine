#pragma once

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
// The JobSystem overload is data-parallel: work items (component pointer +
// angular delta) are snapshotted on the calling thread, then batches write
// to disjoint TransformComponents concurrently — no structural registry
// changes happen while jobs run, which is the thread-safety contract for
// EnTT storage access. parallelFor()'s return is the barrier.
// ---------------------------------------------------------------------------
class RotationSystem {
public:
    static void update(Scene& scene, double fixedDeltaSeconds);
    static void update(Scene& scene, double fixedDeltaSeconds, JobSystem& jobs);
};

} // namespace engine

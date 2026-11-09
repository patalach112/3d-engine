#pragma once

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// RotationSystem
//
// Integrates SpinComponent into TransformComponent rotation. Runs in the
// loop's fixed update, so spinning is deterministic and independent of
// frame rate.
// ---------------------------------------------------------------------------
class RotationSystem {
public:
    static void update(Scene& scene, double fixedDeltaSeconds);
};

} // namespace engine

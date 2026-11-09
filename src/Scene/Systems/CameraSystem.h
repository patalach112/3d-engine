#pragma once
#include "Core/Export.h"

namespace engine {

class Input;
class Scene;

// ---------------------------------------------------------------------------
// CameraSystem
//
// Drives the scene's active CameraComponent from player input: mouse look
// (only while the cursor is captured) and WASD/QE movement, integrated
// with the variable frame delta. Runs in the loop's variable update;
// rendering reads the resulting camera state afterwards, never before.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API CameraSystem {
public:
    static void update(Scene& scene, const Input& input, double deltaSeconds);
};

} // namespace engine

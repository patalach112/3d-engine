#pragma once

// Reflection blocks for the built-in ECS components (Phase 25).
// Include this header wherever compile-time reflect<T>() access to
// these types is needed; Reflection.cpp registers the same types
// with ReflectionRegistry for name-based lookup (editor, console).

#include "Scene/Components.h"
#include "Scene/Reflection.h"

namespace engine {

ENGINE_REFLECT_BEGIN(TransformComponent)
    ENGINE_REFLECT_PROPERTY(position, "Position")
    ENGINE_REFLECT_PROPERTY(rotationDegrees, "Rotation (degrees)")
    ENGINE_REFLECT_PROPERTY(scale, "Scale")
ENGINE_REFLECT_END()

ENGINE_REFLECT_BEGIN(RigidBodyComponent)
    ENGINE_REFLECT_PROPERTY(mass, "Mass (kg)")
    ENGINE_REFLECT_PROPERTY(velocity, "Velocity")
    ENGINE_REFLECT_PROPERTY(angularVelocity, "Angular Velocity")
    ENGINE_REFLECT_PROPERTY(friction, "Friction")
    ENGINE_REFLECT_PROPERTY(restitution, "Restitution")
    ENGINE_REFLECT_PROPERTY(gravityScale, "Gravity Scale")
    ENGINE_REFLECT_PROPERTY(useGravity, "Use Gravity")
    ENGINE_REFLECT_PROPERTY(allowSleep, "Allow Sleep")
ENGINE_REFLECT_END()

ENGINE_REFLECT_BEGIN(PointLightComponent)
    ENGINE_REFLECT_PROPERTY(color, "Color")
    ENGINE_REFLECT_PROPERTY(intensity, "Intensity")
    ENGINE_REFLECT_PROPERTY(range, "Range")
    ENGINE_REFLECT_PROPERTY(enabled, "Enabled")
ENGINE_REFLECT_END()

ENGINE_REFLECT_BEGIN(SpotLightComponent)
    ENGINE_REFLECT_PROPERTY(color, "Color")
    ENGINE_REFLECT_PROPERTY(intensity, "Intensity")
    ENGINE_REFLECT_PROPERTY(range, "Range")
    ENGINE_REFLECT_PROPERTY(innerConeRadians, "Inner Cone (rad)")
    ENGINE_REFLECT_PROPERTY(outerConeRadians, "Outer Cone (rad)")
    ENGINE_REFLECT_PROPERTY(enabled, "Enabled")
    ENGINE_REFLECT_PROPERTY(castShadow, "Cast Shadow")
ENGINE_REFLECT_END()

ENGINE_REFLECT_BEGIN(DirectionalLightComponent)
    ENGINE_REFLECT_PROPERTY(color, "Color")
    ENGINE_REFLECT_PROPERTY(intensity, "Intensity")
    ENGINE_REFLECT_PROPERTY(direction, "Direction (toward light)")
    ENGINE_REFLECT_PROPERTY(enabled, "Enabled")
    ENGINE_REFLECT_PROPERTY(castShadow, "Cast Shadow")
    ENGINE_REFLECT_PROPERTY(cascadeCount, "Shadow Cascades")
ENGINE_REFLECT_END()

ENGINE_REFLECT_BEGIN(SpinComponent)
    ENGINE_REFLECT_PROPERTY(degreesPerSecond, "Degrees / Second")
ENGINE_REFLECT_END()

} // namespace engine

#pragma once
#include "Core/Export.h"

#include <entt/entt.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

#include "Core/Camera.h"
#include "Geometry/Ray.h"
#include "Graphics/UIContext.h"

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// TransformGizmo (Phase 28): interactive 3D transform gizmo for the
// selected entity, in the Unity/Blender tradition:
//
//   * Translate — three axis arrows; dragging slides the entity
//     along exactly that world axis (the other two position
//     components are bit-for-bit frozen),
//   * Rotate — three rings in the planes perpendicular to the
//     axes; dragging spins the entity about exactly that axis
//     (the other Euler components are frozen; the orientation
//     quaternion is re-synced so modelMatrix sees the edit),
//   * Scale — three axis handles; dragging grows/shrinks exactly
//     that scale component, clamped to [kMinScale, kMaxScale] so
//     a drag can never invert or annihilate an entity.
//
// Interaction is pure immediate-mode math over UIContext input:
// handles are picked in screen space (distance from the cursor to
// the projected handle geometry), drags are driven by exact 3D
// constructions (closest point between the mouse ray and the axis
// line; ray/ring-plane intersection angles), and results are
// written straight into the entity's TransformComponent — the same
// fields the reflection-driven Inspector reads every frame, so
// edits show up in the dock instantly. The gizmo operates on the
// LOCAL transform (what the Inspector shows); its anchor is the
// entity's world position so nested selections still sit on the
// rendered entity.
//
// Rendering is procedural screen-space geometry projected from
// the true 3D handle positions and emitted into the UIBatch
// overlay (dotted axis lines, ring outlines, tip glyphs), so the
// gizmo always draws on top of the scene.
//
// Drags lock onto their target at press time; deleting the target
// mid-drag (or switching tools) cancels cleanly.
// ---------------------------------------------------------------------------

enum class GizmoMode : uint8_t { Translate, Rotate, Scale };
enum class GizmoAxis : uint8_t { None, X, Y, Z };

struct GizmoUpdateResult {
    GizmoAxis hoveredAxis = GizmoAxis::None; // handle under the cursor (active axis while dragging)
    bool dragging = false;                   // a drag is in progress (including this frame)
    bool changed = false;                    // the TransformComponent was written this frame
    bool pressConsumed = false;              // this frame's press began on a handle
};

class ENGINE_SCENE_API TransformGizmo {
public:
    static constexpr float kMinScale = 0.01f;          // scale clamp, lower bound
    static constexpr float kMaxScale = 100.0f;         // scale clamp, upper bound
    static constexpr float kHoverThresholdPx = 12.0f;  // handle picking distance in pixels
    static constexpr float kHandleLengthFraction = 0.28f; // of camera distance, before clamping
    static constexpr float kMinHandleLength = 0.4f;
    static constexpr float kMaxHandleLength = 20.0f;

    void setMode(GizmoMode mode); // switching tools cancels any drag
    [[nodiscard]] GizmoMode mode() const { return m_mode; }

    [[nodiscard]] GizmoAxis hoveredAxis() const { return m_hoveredAxis; }
    [[nodiscard]] GizmoAxis activeAxis() const { return m_dragging ? m_axis : GizmoAxis::None; }
    [[nodiscard]] bool dragging() const { return m_dragging; }
    [[nodiscard]] entt::entity dragTarget() const { return m_dragging ? m_target : entt::null; }

    // Runs one frame of gizmo interaction + rendering for
    // `selected` (drawn only when the entity is valid and carries a
    // TransformComponent). `inputBlocked` (popup open, mouse
    // captured, ...) suppresses new drags and cancels a running one
    // while still rendering the handles.
    GizmoUpdateResult update(UIContext& ui, Scene& scene, entt::entity selected, const Camera& camera,
                             glm::vec2 viewportSize, bool inputBlocked = false);

    // World-space unit direction of a gizmo axis (None -> +X fallback).
    [[nodiscard]] static glm::vec3 axisDirection(GizmoAxis axis);
    // Projects a world point to viewport pixels (origin top-left) —
    // the exact inverse of screenToNdc. False when the point is
    // behind the camera or the viewport is degenerate.
    [[nodiscard]] static bool projectToScreen(glm::vec3 worldPosition, const glm::mat4& viewProjection,
                                              glm::vec2 viewportSize, glm::vec2& outPixels);
    // Parameter t of the point on the (infinite) axis line closest
    // to the mouse ray: line point = lineOrigin + lineDirection * t.
    // Robust for parallel rays (falls back to the projected origin).
    [[nodiscard]] static float rayAxisParameter(const Ray& ray, glm::vec3 lineOrigin, glm::vec3 lineDirection);
    // Screen-size-independent handle length for a target at the
    // given distance from the camera, clamped to sane bounds.
    [[nodiscard]] static float handleLength(float cameraDistance);

private:
    GizmoMode m_mode = GizmoMode::Translate;
    GizmoAxis m_hoveredAxis = GizmoAxis::None;
    GizmoAxis m_axis = GizmoAxis::None;
    bool m_dragging = false;
    entt::entity m_target = entt::null;
    // Drag snapshot (taken on the press frame).
    glm::vec3 m_anchor{0.0f};       // world position the drag pivots about
    glm::vec3 m_startPosition{0.0f};
    glm::vec3 m_startRotation{0.0f};
    glm::vec3 m_startScale{1.0f};
    float m_startParameter = 0.0f;  // axis-line parameter at press time
    glm::vec3 m_startPlaneVector{1.0f, 0.0f, 0.0f}; // in-plane reference for Rotate
};

} // namespace engine

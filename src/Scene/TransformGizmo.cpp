#include "Scene/TransformGizmo.h"

#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"
#include "Scene/Raycast.h"
#include "Scene/Scene.h"
#include "Scene/Systems/TransformSystem.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

constexpr GizmoAxis kAxes[3] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};
constexpr int kRingSamples = 64;

int axisIndex(GizmoAxis axis) {
    switch (axis) {
        case GizmoAxis::X: return 0;
        case GizmoAxis::Y: return 1;
        case GizmoAxis::Z: return 2;
        case GizmoAxis::None: break;
    }
    return 0;
}

const char* axisLabel(GizmoAxis axis) {
    switch (axis) {
        case GizmoAxis::X: return "X";
        case GizmoAxis::Y: return "Y";
        case GizmoAxis::Z: return "Z";
        case GizmoAxis::None: break;
    }
    return "";
}

glm::vec4 axisColor(GizmoAxis axis, bool highlighted) {
    if (highlighted) {
        return glm::vec4{1.0f, 0.82f, 0.25f, 0.98f};
    }
    switch (axis) {
        case GizmoAxis::X: return glm::vec4{0.93f, 0.27f, 0.27f, 0.95f};
        case GizmoAxis::Y: return glm::vec4{0.35f, 0.85f, 0.38f, 0.95f};
        case GizmoAxis::Z: return glm::vec4{0.32f, 0.52f, 1.0f, 0.95f};
        case GizmoAxis::None: break;
    }
    return glm::vec4{1.0f};
}

// Orthonormal basis (u, v) of the plane perpendicular to `axis`.
void ringBasis(GizmoAxis axis, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 normal = TransformGizmo::axisDirection(axis);
    const glm::vec3 helper = std::abs(normal.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
    u = glm::normalize(glm::cross(helper, normal));
    v = glm::cross(normal, u);
}

float pointSegmentDistance(glm::vec2 point, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float lengthSq = glm::dot(ab, ab);
    const float t = lengthSq > 0.0f ? std::clamp(glm::dot(point - a, ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
    return glm::distance(point, a + ab * t);
}

bool rayPlanePoint(const Ray& ray, glm::vec3 center, glm::vec3 normal, glm::vec3& outPoint) {
    const float denom = glm::dot(ray.direction, normal);
    if (std::abs(denom) < 1e-8f) {
        return false; // ray runs parallel to the plane
    }
    const float t = glm::dot(center - ray.origin, normal) / denom;
    if (t < 0.0f) {
        return false; // plane behind the camera
    }
    outPoint = ray.at(t);
    return true;
}

float wrapDegrees(float degrees) {
    float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    return wrapped - 180.0f;
}

void drawDot(UIBatch& batch, glm::vec2 center, float radius, glm::vec4 color) {
    batch.drawRoundedRect(center.x - radius, center.y - radius, 2.0f * radius, 2.0f * radius,
                          UIRectStyle{radius, 0.0f, glm::vec4{0.0f}}, color);
}

void drawDottedSegment(UIBatch& batch, glm::vec2 a, glm::vec2 b, glm::vec4 color, float radius) {
    const float length = glm::distance(a, b);
    const int steps = std::max(1, static_cast<int>(length / 6.0f));
    for (int i = 0; i <= steps; ++i) {
        drawDot(batch, a + (b - a) * (static_cast<float>(i) / static_cast<float>(steps)), radius, color);
    }
}

// Projected handle geometry for one axis in the current mode.
struct HandleProjection {
    GizmoAxis axis = GizmoAxis::None;
    glm::vec2 center{0.0f};
    bool centerValid = false;
    glm::vec2 tip{0.0f}; // translate/scale handle end
    bool tipValid = false;
    std::array<glm::vec2, kRingSamples> ring{};
    std::array<bool, kRingSamples> ringValid{};
};

HandleProjection projectHandle(GizmoMode mode, GizmoAxis axis, glm::vec3 anchor, float length,
                               const glm::mat4& viewProjection, glm::vec2 viewportSize) {
    HandleProjection handle;
    handle.axis = axis;
    handle.centerValid = TransformGizmo::projectToScreen(anchor, viewProjection, viewportSize, handle.center);
    if (mode == GizmoMode::Rotate) {
        glm::vec3 u;
        glm::vec3 v;
        ringBasis(axis, u, v);
        for (int i = 0; i < kRingSamples; ++i) {
            const float angle = 2.0f * 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(kRingSamples);
            const glm::vec3 point = anchor + (u * std::cos(angle) + v * std::sin(angle)) * length;
            handle.ringValid[static_cast<size_t>(i)] =
                TransformGizmo::projectToScreen(point, viewProjection, viewportSize, handle.ring[static_cast<size_t>(i)]);
        }
    } else {
        const glm::vec3 tipWorld = anchor + TransformGizmo::axisDirection(axis) * length;
        handle.tipValid = TransformGizmo::projectToScreen(tipWorld, viewProjection, viewportSize, handle.tip);
    }
    return handle;
}

float handleHoverDistance(const HandleProjection& handle, GizmoMode mode, glm::vec2 mouse) {
    float best = std::numeric_limits<float>::max();
    if (mode == GizmoMode::Rotate) {
        for (int i = 0; i < kRingSamples; ++i) {
            const size_t a = static_cast<size_t>(i);
            const size_t b = static_cast<size_t>((i + 1) % kRingSamples);
            if (handle.ringValid[a] && handle.ringValid[b]) {
                best = std::min(best, pointSegmentDistance(mouse, handle.ring[a], handle.ring[b]));
            }
        }
    } else if (handle.centerValid && handle.tipValid) {
        best = pointSegmentDistance(mouse, handle.center, handle.tip);
    }
    return best;
}

void drawHandle(UIBatch& batch, const HandleProjection& handle, GizmoMode mode, bool highlighted,
                glm::vec2 origin) {
    const glm::vec4 color = axisColor(handle.axis, highlighted);
    const float lineRadius = highlighted ? 3.2f : 2.1f;
    if (mode == GizmoMode::Rotate) {
        glm::vec2 labelPoint{0.0f};
        bool haveLabel = false;
        for (int i = 0; i < kRingSamples; ++i) {
            const size_t index = static_cast<size_t>(i);
            if (!handle.ringValid[index]) {
                continue;
            }
            drawDot(batch, handle.ring[index] + origin, lineRadius, color);
            if (!haveLabel) {
                labelPoint = handle.ring[index] + origin;
                haveLabel = true;
            }
        }
        if (haveLabel) {
            batch.drawText(axisLabel(handle.axis), labelPoint.x + 7.0f, labelPoint.y - 5.0f, 1.4f, color);
        }
        return;
    }
    if (!handle.centerValid || !handle.tipValid) {
        return;
    }
    drawDottedSegment(batch, handle.center + origin, handle.tip + origin, color, lineRadius);
    if (mode == GizmoMode::Scale) {
        const float half = highlighted ? 6.0f : 5.0f;
        const glm::vec2 tip = handle.tip + origin;
        batch.drawRoundedRect(tip.x - half, tip.y - half, 2.0f * half, 2.0f * half,
                              UIRectStyle{2.0f, 0.0f, glm::vec4{0.0f}}, color);
    } else {
        drawDot(batch, handle.tip + origin, highlighted ? 6.5f : 5.5f, color);
    }
    const glm::vec2 tip = handle.tip + origin;
    batch.drawText(axisLabel(handle.axis), tip.x + 8.0f, tip.y - 5.0f, 1.4f, color);
}

} // namespace

glm::vec3 TransformGizmo::axisDirection(GizmoAxis axis) {
    switch (axis) {
        case GizmoAxis::X: return glm::vec3{1.0f, 0.0f, 0.0f};
        case GizmoAxis::Y: return glm::vec3{0.0f, 1.0f, 0.0f};
        case GizmoAxis::Z: return glm::vec3{0.0f, 0.0f, 1.0f};
        case GizmoAxis::None: break;
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
}

bool TransformGizmo::projectToScreen(glm::vec3 worldPosition, const glm::mat4& viewProjection,
                                     glm::vec2 viewportSize, glm::vec2& outPixels) {
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) {
        return false;
    }
    const glm::vec4 clip = viewProjection * glm::vec4{worldPosition, 1.0f};
    if (clip.w <= 1e-6f) {
        return false; // behind the camera (or on its plane)
    }
    const glm::vec3 ndc = glm::vec3{clip} / clip.w;
    outPixels = glm::vec2{(ndc.x + 1.0f) * 0.5f * viewportSize.x, (ndc.y + 1.0f) * 0.5f * viewportSize.y};
    return true;
}

float TransformGizmo::rayAxisParameter(const Ray& ray, glm::vec3 lineOrigin, glm::vec3 lineDirection) {
    // Closest-point parameter between the mouse ray and the axis
    // line (both directions unit length). Falls back to the
    // projected ray origin when the two are (near-)parallel.
    const glm::vec3 w0 = ray.origin - lineOrigin;
    const float b = glm::dot(ray.direction, lineDirection);
    const float d = glm::dot(ray.direction, w0);
    const float e = glm::dot(lineDirection, w0);
    const float denom = 1.0f - b * b;
    if (std::abs(denom) < 1e-6f) {
        return e;
    }
    return (e - b * d) / denom;
}

float TransformGizmo::handleLength(float cameraDistance) {
    return std::clamp(cameraDistance * kHandleLengthFraction, kMinHandleLength, kMaxHandleLength);
}

bool TransformGizmo::selectionBoxCorners(const Scene& scene, entt::entity entity,
                                         std::array<glm::vec3, 8>& outCorners) {
    if (entity == entt::null) {
        return false;
    }
    const Scene::Registry& registry = scene.registry();
    if (!registry.valid(entity)) {
        return false;
    }
    const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
    if (transform == nullptr) {
        return false;
    }
    const glm::mat4 model = TransformSystem::effectiveWorldMatrix(
        *transform, registry.try_get<WorldTransformComponent>(entity));

    // The pick volume picking itself uses (Scene::syncRaycastTree):
    // a visible mesh's bounds and/or a box/OBB collider, unioned.
    bool hasBox = false;
    AABB localBox{};
    if (const MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        mesh != nullptr && mesh->visible) {
        localBox = mesh->localBounds;
        hasBox = true;
    }
    if (const ColliderComponent* collider = registry.try_get<ColliderComponent>(entity);
        collider != nullptr &&
        (collider->type == ColliderComponent::Type::Box || collider->type == ColliderComponent::Type::Obb)) {
        const AABB colliderBox{collider->centerOffset - glm::abs(collider->halfExtents),
                               collider->centerOffset + glm::abs(collider->halfExtents)};
        localBox = hasBox ? localBox.combined(colliderBox) : colliderBox;
        hasBox = true;
    }
    if (!hasBox) {
        // Nothing pick-shaped on this entity (lights, cameras, plain
        // transforms): still outline it, with a modest box at its
        // position, so no selection is ever invisible.
        localBox = AABB{glm::vec3{-0.25f}, glm::vec3{0.25f}};
    }
    for (size_t i = 0; i < outCorners.size(); ++i) {
        const glm::vec3 corner{(i & 1u) != 0u ? localBox.max.x : localBox.min.x,
                               (i & 2u) != 0u ? localBox.max.y : localBox.min.y,
                               (i & 4u) != 0u ? localBox.max.z : localBox.min.z};
        outCorners[i] = glm::vec3{model * glm::vec4{corner, 1.0f}};
    }
    return true;
}

void TransformGizmo::drawSelectionOutline(UIBatch& batch, const Scene& scene, entt::entity entity,
                                          const Camera& camera, const UIRect& viewport) {
    if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return;
    }
    std::array<glm::vec3, 8> corners{};
    if (!selectionBoxCorners(scene, entity, corners)) {
        return;
    }
    const glm::vec2 viewportSize{viewport.width, viewport.height};
    const glm::vec2 viewportOrigin{viewport.x, viewport.y};
    const glm::mat4 viewProjection =
        camera.projectionMatrix(viewportSize.x / viewportSize.y) * camera.viewMatrix();

    std::array<glm::vec2, 8> pixels{};
    std::array<bool, 8> valid{};
    for (size_t i = 0; i < corners.size(); ++i) {
        valid[i] = projectToScreen(corners[i], viewProjection, viewportSize, pixels[i]);
        if (valid[i]) {
            pixels[i] += viewportOrigin;
        }
    }
    // Editor amber; brighter than the gizmo's neutral centre dot so
    // the two never read as one widget.
    const glm::vec4 outlineColor{1.0f, 0.72f, 0.15f, 0.95f};
    constexpr std::array<std::pair<size_t, size_t>, 12> kEdges{{
        {0, 1}, {2, 3}, {4, 5}, {6, 7}, // x-running
        {0, 2}, {1, 3}, {4, 6}, {5, 7}, // y-running
        {0, 4}, {1, 5}, {2, 6}, {3, 7}, // z-running
    }};
    for (const auto& [a, b] : kEdges) {
        if (valid[a] && valid[b]) {
            drawDottedSegment(batch, pixels[a], pixels[b], outlineColor, 1.7f);
        }
    }
    for (size_t i = 0; i < corners.size(); ++i) {
        if (valid[i]) {
            drawDot(batch, pixels[i], 2.6f, outlineColor);
        }
    }
}

void TransformGizmo::setMode(GizmoMode mode) {
    if (mode != m_mode) {
        m_mode = mode;
        m_dragging = false; // switching tools drops the drag
        m_axis = GizmoAxis::None;
    }
}

GizmoUpdateResult TransformGizmo::update(UIContext& ui, Scene& scene, entt::entity selected, const Camera& camera,
                                         glm::vec2 viewportSize, bool inputBlocked) {
    return update(ui, scene, selected, camera, UIRect{0.0f, 0.0f, viewportSize.x, viewportSize.y},
                  inputBlocked);
}

GizmoUpdateResult TransformGizmo::update(UIContext& ui, Scene& scene, entt::entity selected, const Camera& camera,
                                         const UIRect& viewport, bool inputBlocked) {
    GizmoUpdateResult result;
    UIBatch* batch = ui.batch();
    const glm::vec2 viewportSize{viewport.width, viewport.height};
    const glm::vec2 viewportOrigin{viewport.x, viewport.y};
    if (batch == nullptr || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) {
        m_dragging = false;
        return result;
    }
    auto& registry = scene.registry();

    if (inputBlocked) {
        m_dragging = false; // a popup/capture interrupts the drag
    }

    // The drag target is locked at press time; otherwise the gizmo
    // follows the current selection.
    const entt::entity target = m_dragging ? m_target : selected;
    TransformComponent* transform = nullptr;
    if (target != entt::null && registry.valid(target)) {
        transform = registry.try_get<TransformComponent>(target);
    }
    if (transform == nullptr) {
        m_dragging = false;
        m_hoveredAxis = GizmoAxis::None;
        return result;
    }

    const float aspectRatio = viewportSize.x / viewportSize.y;
    const glm::mat4 viewProjection = camera.projectionMatrix(aspectRatio) * camera.viewMatrix();
    const glm::vec3 anchor = registry.try_get<WorldTransformComponent>(target) != nullptr
                                 ? registry.get<WorldTransformComponent>(target).worldPosition
                                 : transform->position;
    const float length = handleLength(glm::distance(camera.position(), anchor));

    const glm::vec2 rawMouse = ui.mousePosition();
    const bool hasPointer = rawMouse.x > -9000.0f; // parked off-screen while captured
    // All gizmo math (projection, hover, rays) is viewport-local; only
    // drawing translates back by the panel origin.
    const glm::vec2 mouse = rawMouse - viewportOrigin;
    if (!hasPointer) {
        m_dragging = false;
    }

    std::array<HandleProjection, 3> handles;
    for (int i = 0; i < 3; ++i) {
        handles[static_cast<size_t>(i)] =
            projectHandle(m_mode, kAxes[i], anchor, length, viewProjection, viewportSize);
    }

    // --- Hover picking (screen space) --------------------------------------
    GizmoAxis hover = GizmoAxis::None;
    if (hasPointer && !m_dragging) {
        float bestDistance = kHoverThresholdPx;
        for (const HandleProjection& handle : handles) {
            const float distance = handleHoverDistance(handle, m_mode, mouse);
            if (distance < bestDistance) { // strict: first axis wins ties deterministically
                bestDistance = distance;
                hover = handle.axis;
            }
        }
    }
    m_hoveredAxis = m_dragging ? m_axis : hover;

    const glm::vec3 axisDir = axisDirection(m_axis != GizmoAxis::None ? m_axis : hover);
    const Ray mouseRay = cameraRay(camera, screenToNdc(mouse, viewportSize), aspectRatio);

    // --- Drag start ----------------------------------------------------------
    if (!m_dragging && hover != GizmoAxis::None && hasPointer && !inputBlocked && !ui.popupOpen() &&
        ui.activeId() == kUINoWidget && ui.pressedRect(viewport)) {
        bool started = true;
        if (m_mode == GizmoMode::Rotate) {
            glm::vec3 hitPoint;
            if (rayPlanePoint(mouseRay, anchor, axisDir, hitPoint) &&
                glm::dot(hitPoint - anchor, hitPoint - anchor) > 1e-10f) {
                m_startPlaneVector = glm::normalize(hitPoint - anchor);
            } else {
                started = false; // ring plane edge-on: no meaningful angle
            }
        } else {
            m_startParameter = rayAxisParameter(mouseRay, anchor, axisDir);
        }
        if (started) {
            m_dragging = true;
            m_axis = hover;
            m_target = target;
            m_anchor = anchor;
            m_startPosition = transform->position;
            m_startRotation = transform->rotationDegrees;
            m_startScale = transform->scale;
            result.pressConsumed = true;
        }
    }

    // --- Drag update ---------------------------------------------------------
    if (m_dragging) {
        if (!ui.mouseDown()) {
            m_dragging = false; // released: the drag ends without a final write
        } else if (registry.valid(m_target) && registry.try_get<TransformComponent>(m_target) != nullptr) {
            TransformComponent& live = registry.get<TransformComponent>(m_target);
            const glm::vec3 dragAxis = axisDirection(m_axis);
            const int component = axisIndex(m_axis);
            if (m_mode == GizmoMode::Rotate) {
                glm::vec3 hitPoint;
                if (rayPlanePoint(mouseRay, m_anchor, dragAxis, hitPoint)) {
                    const glm::vec3 toHit = hitPoint - m_anchor;
                    if (glm::dot(toHit, toHit) > 1e-10f) {
                        const glm::vec3 current = glm::normalize(toHit);
                        const float angle = std::atan2(glm::dot(glm::cross(m_startPlaneVector, current), dragAxis),
                                                       glm::dot(m_startPlaneVector, current));
                        glm::vec3 rotation = m_startRotation;
                        rotation[component] = wrapDegrees(m_startRotation[component] + glm::degrees(angle));
                        live.rotationDegrees = rotation;
                        live.syncOrientationFromEuler();
                        result.changed = true;
                    }
                }
            } else {
                const float parameter = rayAxisParameter(mouseRay, m_anchor, dragAxis);
                const float delta = parameter - m_startParameter;
                if (m_mode == GizmoMode::Translate) {
                    glm::vec3 position = m_startPosition;
                    position[component] += delta;
                    live.position = position;
                } else {
                    glm::vec3 scale = live.scale;
                    scale[component] = std::clamp(m_startScale[component] + delta, kMinScale, kMaxScale);
                    live.scale = scale;
                }
                result.changed = true;
            }
            result.dragging = true;
        } else {
            m_dragging = false; // target deleted mid-drag
        }
    }
    result.hoveredAxis = m_dragging ? m_axis : hover;
    m_hoveredAxis = result.hoveredAxis;

    // --- Rendering (procedural, always on top) ------------------------------
    for (int pass = 0; pass < 2; ++pass) {
        for (const HandleProjection& handle : handles) {
            const bool highlighted = handle.axis == result.hoveredAxis;
            if ((pass == 1) == highlighted) {
                drawHandle(*batch, handle, m_mode, highlighted, viewportOrigin);
            }
        }
    }
    if (handles[0].centerValid) {
        const glm::vec2 center = handles[0].center + viewportOrigin;
        batch->drawRoundedRect(center.x - 3.5f, center.y - 3.5f, 7.0f, 7.0f,
                               UIRectStyle{1.5f, 0.0f, glm::vec4{0.0f}}, glm::vec4{0.95f, 0.97f, 1.0f, 0.95f});
    }
    return result;
}

} // namespace engine

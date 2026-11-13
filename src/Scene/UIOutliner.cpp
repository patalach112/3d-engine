#include "Scene/UIOutliner.h"

#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine {
namespace {

std::string entityName(Scene::Registry& registry, entt::entity entity) {
    if (const TagComponent* tag = registry.try_get<TagComponent>(entity); tag != nullptr && !tag->tag.empty()) {
        return tag->tag;
    }
    return "Entity " + std::to_string(entt::to_integral(entity));
}

template <typename T>
void copyComponent(Scene::Registry& registry, entt::entity from, entt::entity to) {
    if (const T* component = registry.try_get<T>(from); component != nullptr) {
        registry.emplace<T>(to, *component);
    }
}

} // namespace

// Duplicates the data components onto a fresh entity (same parent;
// children are NOT duplicated, and a script component is recreated
// fresh so the duplicate never shares a script instance).
entt::entity UIOutliner::duplicateEntity(Scene& scene, entt::entity source) {
    auto& registry = scene.registry();
    if (!registry.valid(source)) {
        return entt::null;
    }
    Entity copy = scene.createEntity(entityName(registry, source) + " copy");
    const entt::entity target = copy.handle();
    copyComponent<TransformComponent>(registry, source, target);
    copyComponent<MeshComponent>(registry, source, target);
    copyComponent<RigidBodyComponent>(registry, source, target);
    copyComponent<ColliderComponent>(registry, source, target);
    copyComponent<SpinComponent>(registry, source, target);
    copyComponent<PointLightComponent>(registry, source, target);
    copyComponent<SpotLightComponent>(registry, source, target);
    copyComponent<FlashlightComponent>(registry, source, target);
    if (const NativeScriptComponent* script = registry.try_get<NativeScriptComponent>(source);
        script != nullptr) {
        registry.emplace<NativeScriptComponent>(target, script->scriptName);
    }
    if (const ParentComponent* parent = registry.try_get<ParentComponent>(source); parent != nullptr &&
                                                                                    registry.valid(parent->parent)) {
        scene.setParent(copy, Entity{parent->parent, &scene});
    }
    return target;
}

entt::entity UIOutliner::createChildEntity(Scene& scene, entt::entity parent) {
    if (!scene.registry().valid(parent)) {
        return entt::null;
    }
    Entity child = scene.createEntity("Entity");
    scene.setParent(child, Entity{parent, &scene});
    return child.handle();
}

void UIOutliner::resetEntityTransform(Scene& scene, entt::entity entity) {
    auto& registry = scene.registry();
    if (!registry.valid(entity)) {
        return;
    }
    if (TransformComponent* transform = registry.try_get<TransformComponent>(entity);
        transform != nullptr) {
        transform->position = glm::vec3{0.0f};
        transform->rotationDegrees = glm::vec3{0.0f};
        transform->scale = glm::vec3{1.0f};
        transform->syncOrientationFromEuler(); // keep the quaternion in step
    }
}

UIOutliner::Result UIOutliner::build(UIContext& ui, const UIRect& rect, Scene& scene) {
    Result result;
    UIBatch* batch = ui.batch();
    if (batch == nullptr) {
        return result;
    }
    auto& registry = scene.registry();
    if (m_selected != entt::null && !registry.valid(m_selected)) {
        m_selected = entt::null; // deleted directly or via a cascade
    }

    // Dock panels tile flush (square, borderless): neighbours butt
    // edge-to-edge, so rounded corners would notch the seams.
    ui.beginPanel("Scene Hierarchy", rect, UIRectStyle{-1.0f, -1.0f, glm::vec4{}});
    ui.label("Scene Hierarchy");

    // Flatten the hierarchy: roots sorted by name (deterministic),
    // children in scene order, depth-capped and cycle-guarded.
    struct Row {
        entt::entity entity;
        int depth;
    };
    std::vector<Row> rows;
    {
        std::vector<entt::entity> roots;
        for (const entt::entity entity : registry.view<TagComponent>()) {
            if (!registry.all_of<ParentComponent>(entity)) {
                roots.push_back(entity);
            }
        }
        std::sort(roots.begin(), roots.end(), [&](entt::entity a, entt::entity b) {
            const std::string nameA = entityName(registry, a);
            const std::string nameB = entityName(registry, b);
            return nameA != nameB ? nameA < nameB : entt::to_integral(a) < entt::to_integral(b);
        });
        std::unordered_set<entt::entity> visited;
        const std::function<void(entt::entity, int)> collect = [&](entt::entity entity, int depth) {
            if (!registry.valid(entity) || depth > 32 || !visited.insert(entity).second) {
                return;
            }
            rows.push_back(Row{entity, depth});
            if (const ChildrenComponent* children = registry.try_get<ChildrenComponent>(entity);
                children != nullptr) {
                for (const entt::entity child : children->children) {
                    collect(child, depth + 1);
                }
            }
        };
        for (const entt::entity root : roots) {
            collect(root, 0);
        }
        // Anything unreachable (shouldn't happen) still lists, at root.
        for (const entt::entity entity : registry.view<TagComponent>()) {
            if (!visited.contains(entity)) {
                rows.push_back(Row{entity, 0});
            }
        }
    }

    const float rowsTop = rect.y + UIContext::kPanelPadding +
                          (UIContext::kRowHeight + UIContext::kRowSpacing); // below the header row
    const float viewBottom = rect.y + rect.height - UIContext::kPanelPadding;
    const float viewHeight = std::max(0.0f, viewBottom - rowsTop);
    const float contentHeight = static_cast<float>(rows.size()) * kRowHeight;
    if (ui.hoveredRect(rect)) {
        m_scroll -= ui.scrollDelta() * (3.0f * kRowHeight);
    }
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, contentHeight - viewHeight));

    if (rows.empty()) {
        batch->drawText("No entities", rect.x + UIContext::kPanelPadding + 2.0f, rowsTop + 2.0f,
                        UIContext::kTextScale, glm::vec4{0.55f, 0.60f, 0.70f, 1.0f});
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const UIRect rowRect{rect.x + 4.0f, rowsTop + static_cast<float>(i) * kRowHeight - m_scroll,
                             rect.width - 8.0f, kRowHeight};
        if (rowRect.y + kRowHeight < rowsTop || rowRect.y > viewBottom) {
            continue; // fully scrolled out of the panel
        }
        const bool isSelected = row.entity == m_selected;
        if (isSelected) {
            batch->drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   UIRectStyle{4.0f, 0.0f, {}}, glm::vec4{0.035f, 0.278f, 0.443f, 0.9f});
        } else if (ui.hoveredRect(rowRect)) {
            batch->drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height,
                                   UIRectStyle{4.0f, 0.0f, {}}, glm::vec4{1.0f, 1.0f, 1.0f, 0.08f});
        }
        const float textX = rowRect.x + 6.0f + static_cast<float>(row.depth) * 14.0f;
        batch->drawText(entityName(registry, row.entity), textX,
                        rowRect.y + 0.5f * (kRowHeight - 7.0f * UIContext::kTextScale),
                        UIContext::kTextScale,
                        isSelected ? glm::vec4{1.0f} : glm::vec4{0.85f, 0.90f, 0.97f, 1.0f});

        if (ui.pressedRect(rowRect) && m_selected != row.entity) {
            m_selected = row.entity;
            result.selectionChanged = true;
        }
        if (ui.rightPressedRect(rowRect)) {
            if (m_selected != row.entity) {
                m_selected = row.entity;
                result.selectionChanged = true;
            }
            m_contextTarget = row.entity;
            const glm::vec2 mouse = ui.mousePosition();
            ui.openPopup("EntityContextMenu", mouse.x, mouse.y);
        }
    }
    ui.endPanel();

    // The context menu renders after every panel, on top, clipped
    // only by the viewport — never by the outliner's own panel.
    if (m_contextTarget != entt::null && !registry.valid(m_contextTarget)) {
        m_contextTarget = entt::null;
    }
    if (ui.beginPopup("EntityContextMenu")) {
        if (ui.menuItem("Duplicate")) {
            result.entity = duplicateEntity(scene, m_contextTarget);
            result.action = Action::Duplicated;
            if (result.entity != entt::null) {
                m_selected = result.entity;
                result.selectionChanged = true;
            }
        }
        if (ui.menuItem("Create Child")) {
            result.entity = createChildEntity(scene, m_contextTarget);
            result.action = Action::CreatedChild;
            if (result.entity != entt::null) {
                m_selected = result.entity;
                result.selectionChanged = true;
            }
        }
        ui.menuSeparator();
        if (ui.menuItem("Delete", m_contextTarget != entt::null)) {
            const entt::entity target = m_contextTarget;
            scene.destroyEntity(Entity{target, &scene});
            result.action = Action::Deleted;
            result.entity = target;
            m_contextTarget = entt::null;
            if (m_selected != entt::null && !registry.valid(m_selected)) {
                m_selected = entt::null;
                result.selectionChanged = true;
            }
        }
        ui.menuSeparator();
        // Rename and Focus are host work (text field / camera), so
        // they are only signalled; Reset Transform edits in place.
        if (ui.menuItem("Rename", m_contextTarget != entt::null)) {
            result.action = Action::RenameRequested;
            result.entity = m_contextTarget;
        }
        if (ui.menuItem("Reset Transform",
                        m_contextTarget != entt::null &&
                            registry.try_get<TransformComponent>(m_contextTarget) != nullptr)) {
            resetEntityTransform(scene, m_contextTarget);
            result.action = Action::TransformReset;
            result.entity = m_contextTarget;
        }
        if (ui.menuItem("Focus", m_contextTarget != entt::null)) {
            result.action = Action::FocusRequested;
            result.entity = m_contextTarget;
        }
        ui.endPopup();
    }
    return result;
}

} // namespace engine

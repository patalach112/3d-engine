#include "Scene/Systems/TransformSystem.h"

#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <unordered_set>
#include <vector>

namespace engine {

glm::mat4 TransformSystem::effectiveWorldMatrix(const TransformComponent& transform,
                                                const WorldTransformComponent* world) {
    return world != nullptr ? world->worldMatrix : transform.modelMatrix();
}

void TransformSystem::decompose(const glm::mat4& matrix, glm::vec3& outPosition, glm::quat& outOrientation,
                                glm::vec3& outScale) {
    outPosition = glm::vec3{matrix[3]};
    const glm::vec3 columnX{matrix[0]};
    const glm::vec3 columnY{matrix[1]};
    const glm::vec3 columnZ{matrix[2]};
    outScale = glm::vec3{glm::length(columnX), glm::length(columnY), glm::length(columnZ)};
    glm::mat3 rotation{1.0f};
    rotation[0] = outScale.x > 1e-12f ? columnX / outScale.x : glm::vec3{1.0f, 0.0f, 0.0f};
    rotation[1] = outScale.y > 1e-12f ? columnY / outScale.y : glm::vec3{0.0f, 1.0f, 0.0f};
    rotation[2] = outScale.z > 1e-12f ? columnZ / outScale.z : glm::vec3{0.0f, 0.0f, 1.0f};
    // A mirrored basis (negative determinant) flips one axis's sign;
    // folding it into the scale keeps the quaternion a pure rotation.
    if (glm::determinant(rotation) < 0.0f) {
        rotation[0] = -rotation[0];
        outScale.x = -outScale.x;
    }
    outOrientation = glm::normalize(glm::quat_cast(rotation));
}

size_t TransformSystem::update(Scene& scene) {
    auto& registry = scene.registry();

    // Ensure every transform has a world-transform slot first, so the
    // walk below only ever writes through pointers. (Structural
    // changes happen outside any view iteration.)
    {
        std::vector<entt::entity> missing;
        const auto transforms = registry.view<TransformComponent>(entt::exclude<WorldTransformComponent>);
        for (const entt::entity entity : transforms) {
            missing.push_back(entity);
        }
        for (const entt::entity entity : missing) {
            registry.emplace<WorldTransformComponent>(entity);
        }
    }

    const auto transforms = registry.view<TransformComponent, WorldTransformComponent>();
    // Keyed by entity integral (version bits included): unique per
    // live handle, and a set — unlike a dense array — tolerates the
    // sparse integral space recycling creates.
    std::unordered_set<uint32_t> visited;
    const auto markVisited = [&visited](entt::entity entity) { visited.insert(entt::to_integral(entity)); };
    const auto wasVisited = [&visited](entt::entity entity) {
        return visited.find(entt::to_integral(entity)) != visited.end();
    };

    size_t updated = 0;
    // Iterative DFS from every root. Children come from
    // ChildrenComponent; a stack entry carries the parent's world
    // matrix down so each entity composes exactly once.
    std::vector<std::pair<entt::entity, glm::mat4>> stack;
    const auto solveSubtree = [&](entt::entity root, const glm::mat4& rootParentWorld) {
        stack.emplace_back(root, rootParentWorld);
        while (!stack.empty()) {
            const auto [entity, parentWorld] = stack.back();
            stack.pop_back();
            if (wasVisited(entity) || !transforms.contains(entity)) {
                continue;
            }
            markVisited(entity);
            WorldTransformComponent& world = transforms.get<WorldTransformComponent>(entity);
            world.worldMatrix = parentWorld * transforms.get<TransformComponent>(entity).modelMatrix();
            decompose(world.worldMatrix, world.worldPosition, world.worldOrientation, world.worldScale);
            ++updated;
            if (const ChildrenComponent* children = registry.try_get<ChildrenComponent>(entity);
                children != nullptr) {
                for (const entt::entity child : children->children) {
                    stack.emplace_back(child, world.worldMatrix);
                }
            }
        }
    };

    for (const entt::entity entity : transforms) {
        const ParentComponent* parent = registry.try_get<ParentComponent>(entity);
        const bool isRoot = parent == nullptr || parent->parent == entt::null ||
                            !registry.valid(parent->parent) ||
                            registry.try_get<TransformComponent>(parent->parent) == nullptr;
        if (isRoot) {
            solveSubtree(entity, glm::mat4{1.0f});
        }
    }
    // Anything still unvisited sits under a parent cycle (only
    // reachable by patching components directly): fall back to the
    // local matrix so the pass always completes.
    for (const entt::entity entity : transforms) {
        if (!wasVisited(entity)) {
            WorldTransformComponent& world = transforms.get<WorldTransformComponent>(entity);
            world.worldMatrix = transforms.get<TransformComponent>(entity).modelMatrix();
            decompose(world.worldMatrix, world.worldPosition, world.worldOrientation, world.worldScale);
            ++updated;
        }
    }
    return updated;
}

} // namespace engine

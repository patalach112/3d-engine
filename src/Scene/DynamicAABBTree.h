#pragma once
#include "Core/Export.h"

#include "Geometry/Bounds.h"
#include "Geometry/Ray.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// DynamicAABBTree
//
// A dynamic bounding-volume hierarchy over fat AABBs (the classic
// Box2D-style broadphase tree), used to accelerate physics pair
// generation and scene raycast picking:
//
//   * Every leaf holds one entity's world AABB, expanded by a small
//     fat margin (plus a prediction along the last reported
//     displacement), so small motions are absorbed without touching
//     the tree; only when a leaf's real box outgrows its fat box is
//     the leaf removed and reinserted (moveLeaf).
//   * Insertion picks the sibling that minimizes the surface-area
//     heuristic (SAH) cost increase, and the walk back to the root
//     refits ancestors and applies SAH rotations wherever a subtree
//     is unbalanced, keeping queries at O(log N) expected cost.
//   * Nodes live in one index-addressed pool with a free list, so
//     ProxyIds (leaf node indices) are stable across pool growth and
//     never alias a live leaf while issued.
//
// Query callbacks: query() callbacks return bool (false stops the
// traversal early). raycast() callbacks return float — the new
// maximum distance to consider — which prunes farther subtrees as
// soon as a close hit is known (return the ray's initial maxDistance
// to never prune; return 0 to stop entirely).
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API DynamicAABBTree {
public:
    using ProxyId = int32_t;
    static constexpr ProxyId kNullProxy = -1;

    DynamicAABBTree() = default;

    // Adds a leaf for `entity` around the (tight) world box and
    // returns its proxy id. The stored box is the fat box: `box`
    // expanded by the fat margin on every side.
    ProxyId insertLeaf(const AABB& box, entt::entity entity);
    // Removes a leaf. The proxy id must not be used afterwards.
    void removeLeaf(ProxyId proxyId);
    // Moves a leaf to a new tight box. Does nothing (returns false)
    // while the new box and its displacement-predicted reach stay
    // inside the current fat box; otherwise the leaf is reinserted
    // around a fresh fat box that also reaches along `displacement`
    // (returns true).
    bool moveLeaf(ProxyId proxyId, const AABB& box, const glm::vec3& displacement);

    // The entity a leaf was inserted for, and its current fat box.
    [[nodiscard]] entt::entity entity(ProxyId proxyId) const;
    [[nodiscard]] const AABB& proxyAabb(ProxyId proxyId) const;

    [[nodiscard]] size_t leafCount() const { return m_leafCount; }
    // Node slots currently in use (leaves + internal nodes).
    [[nodiscard]] size_t nodeCount() const { return m_nodeCount; }
    // Height of the root (a lone leaf is 0; empty tree is 0). A
    // balanced tree over N leaves stays within ~2*log2(N).
    [[nodiscard]] int height() const;

    // Calls `callback(entity)` for every leaf whose fat box overlaps
    // `queryBox`. Traversal stops early when the callback returns
    // false.
    template <typename Callback>
    void query(const AABB& queryBox, Callback&& callback) const {
        if (m_root == kNullProxy) {
            return;
        }
        std::vector<int32_t> stack;
        stack.push_back(m_root);
        while (!stack.empty()) {
            const int32_t nodeIndex = stack.back();
            stack.pop_back();
            const Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
            if (!node.box.overlaps(queryBox)) {
                continue;
            }
            if (node.isLeaf()) {
                if (!callback(node.entity)) {
                    return;
                }
            } else {
                stack.push_back(node.child1);
                stack.push_back(node.child2);
            }
        }
    }

    // Calls `callback(entity)` for every leaf whose fat box the ray
    // pierces within maxDistance (leaves are visited in tree order,
    // not sorted). The callback returns the new maximum distance to
    // consider: returning a found hit's distance prunes all subtrees
    // whose boxes start farther away, returning 0 stops the whole
    // traversal. Subtrees the ray misses are never descended.
    template <typename Callback>
    void raycast(const Ray& ray, float maxDistance, Callback&& callback) const {
        if (m_root == kNullProxy) {
            return;
        }
        float clip = maxDistance;
        std::vector<int32_t> stack;
        stack.push_back(m_root);
        float distance = 0.0f;
        glm::vec3 normal{0.0f};
        while (!stack.empty()) {
            const int32_t nodeIndex = stack.back();
            stack.pop_back();
            const Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
            if (!rayIntersectsAABB(ray, node.box, distance, normal) || distance > clip) {
                continue;
            }
            if (node.isLeaf()) {
                const float newClip = callback(node.entity);
                if (newClip <= 0.0f) {
                    return;
                }
                clip = std::min(clip, newClip);
            } else {
                stack.push_back(node.child1);
                stack.push_back(node.child2);
            }
        }
    }

    // Structural self-check (parent/child links, stored heights,
    // internal boxes equal to the union of their children, reachable
    // node accounting). O(N); intended for tests and debugging.
    [[nodiscard]] bool validate() const;

    // Removes every leaf and resets the pool.
    void clear();

private:
    static constexpr int32_t kNullNode = -1;
    // Fat margin added around every tight box, and the multiplier
    // applied to a reported displacement when extending a fresh fat
    // box toward the predicted next position.
    static constexpr float kFatExtension = 0.1f;
    static constexpr float kDisplacementMultiplier = 2.0f;

    struct Node {
        AABB box{};
        entt::entity entity = entt::null; // payload (leaves only)
        int32_t parent = kNullNode;
        int32_t child1 = kNullNode;
        int32_t child2 = kNullNode;
        int32_t height = 0; // leaves are 0
        int32_t next = kNullNode; // free-list link when unused

        [[nodiscard]] bool isLeaf() const { return child1 == kNullNode; }
    };

    [[nodiscard]] int32_t allocateNode();
    void freeNode(int32_t nodeIndex);
    void insertLeafNode(int32_t leafIndex);
    void removeLeafNode(int32_t leafIndex);
    // Refit/rebalance the subtree rooted at `index` after a change
    // below it; returns the (possibly new) subtree root.
    int32_t balance(int32_t index);
    [[nodiscard]] int32_t computeHeight(int32_t nodeIndex) const;

    static AABB fatten(const AABB& box, const glm::vec3& displacement);

    std::vector<Node> m_nodes;
    int32_t m_root = kNullNode;
    int32_t m_freeNode = kNullNode;
    size_t m_nodeCount = 0;
    size_t m_leafCount = 0;
};

} // namespace engine

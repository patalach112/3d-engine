#include "Scene/DynamicAABBTree.h"

#include <glm/common.hpp>

#include <algorithm>

namespace engine {

namespace {
bool boxesEqual(const AABB& a, const AABB& b) {
    return a.min == b.min && a.max == b.max;
}
} // namespace

AABB DynamicAABBTree::fatten(const AABB& box, const glm::vec3& displacement) {
    AABB fat = box;
    fat.min -= glm::vec3{kFatExtension};
    fat.max += glm::vec3{kFatExtension};
    // Reach along the predicted travel so a body moving steadily does
    // not outgrow its fat box every single step.
    const glm::vec3 reach = displacement * kDisplacementMultiplier;
    fat.min += glm::min(reach, glm::vec3{0.0f});
    fat.max += glm::max(reach, glm::vec3{0.0f});
    return fat;
}

int32_t DynamicAABBTree::allocateNode() {
    if (m_freeNode == kNullNode) {
        const size_t oldCapacity = m_nodes.size();
        const size_t newCapacity = oldCapacity == 0 ? 16 : oldCapacity * 2;
        m_nodes.resize(newCapacity);
        for (size_t i = oldCapacity; i < newCapacity; ++i) {
            m_nodes[i].next = (i + 1 < newCapacity) ? static_cast<int32_t>(i + 1) : kNullNode;
        }
        m_freeNode = static_cast<int32_t>(oldCapacity);
    }
    const int32_t nodeIndex = m_freeNode;
    m_freeNode = m_nodes[static_cast<size_t>(nodeIndex)].next;
    ++m_nodeCount;
    Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
    node.box = AABB{};
    node.entity = entt::null;
    node.parent = kNullNode;
    node.child1 = kNullNode;
    node.child2 = kNullNode;
    node.height = 0;
    node.next = kNullNode;
    return nodeIndex;
}

void DynamicAABBTree::freeNode(int32_t nodeIndex) {
    Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
    node.next = m_freeNode;
    node.height = -1; // poisoned: a freed node must never pass validate()
    m_freeNode = nodeIndex;
    --m_nodeCount;
}

DynamicAABBTree::ProxyId DynamicAABBTree::insertLeaf(const AABB& box, entt::entity entity) {
    const int32_t leaf = allocateNode();
    m_nodes[static_cast<size_t>(leaf)].box = fatten(box, glm::vec3{0.0f});
    m_nodes[static_cast<size_t>(leaf)].entity = entity;
    insertLeafNode(leaf);
    ++m_leafCount;
    return leaf;
}

void DynamicAABBTree::insertLeafNode(int32_t leaf) {
    if (m_root == kNullNode) {
        m_root = leaf;
        m_nodes[static_cast<size_t>(leaf)].parent = kNullNode;
        return;
    }
    // Find the best sibling for the new leaf: descend the tree always
    // following the child whose union with the leaf box costs less
    // additional surface area (the SAH cost of making this internal
    // node the new parent is the break-even candidate at each level).
    const AABB leafBox = m_nodes[static_cast<size_t>(leaf)].box;
    int32_t index = m_root;
    while (!m_nodes[static_cast<size_t>(index)].isLeaf()) {
        const int32_t child1 = m_nodes[static_cast<size_t>(index)].child1;
        const int32_t child2 = m_nodes[static_cast<size_t>(index)].child2;
        const float area = m_nodes[static_cast<size_t>(index)].box.surfaceArea();
        const float combinedArea = m_nodes[static_cast<size_t>(index)].box.combined(leafBox).surfaceArea();
        const float cost = 2.0f * combinedArea;
        const float inheritanceCost = 2.0f * (combinedArea - area);
        const auto descendCost = [this, &leafBox, inheritanceCost](int32_t child) {
            const float combinedChildArea =
                m_nodes[static_cast<size_t>(child)].box.combined(leafBox).surfaceArea();
            if (m_nodes[static_cast<size_t>(child)].isLeaf()) {
                return combinedChildArea + inheritanceCost;
            }
            const float childArea = m_nodes[static_cast<size_t>(child)].box.surfaceArea();
            return (combinedChildArea - childArea) + inheritanceCost;
        };
        const float cost1 = descendCost(child1);
        const float cost2 = descendCost(child2);
        if (cost < cost1 && cost < cost2) {
            break;
        }
        index = cost1 < cost2 ? child1 : child2;
    }
    const int32_t sibling = index;
    const int32_t oldParent = m_nodes[static_cast<size_t>(sibling)].parent;
    const int32_t newParent = allocateNode();
    m_nodes[static_cast<size_t>(newParent)].parent = oldParent;
    m_nodes[static_cast<size_t>(newParent)].box =
        leafBox.combined(m_nodes[static_cast<size_t>(sibling)].box);
    m_nodes[static_cast<size_t>(newParent)].height = m_nodes[static_cast<size_t>(sibling)].height + 1;
    m_nodes[static_cast<size_t>(newParent)].child1 = sibling;
    m_nodes[static_cast<size_t>(newParent)].child2 = leaf;
    m_nodes[static_cast<size_t>(sibling)].parent = newParent;
    m_nodes[static_cast<size_t>(leaf)].parent = newParent;
    if (oldParent != kNullNode) {
        if (m_nodes[static_cast<size_t>(oldParent)].child1 == sibling) {
            m_nodes[static_cast<size_t>(oldParent)].child1 = newParent;
        } else {
            m_nodes[static_cast<size_t>(oldParent)].child2 = newParent;
        }
    } else {
        m_root = newParent;
    }
    // Walk back to the root, rotating unbalanced subtrees and
    // refitting every ancestor's box and height.
    int32_t walk = m_nodes[static_cast<size_t>(leaf)].parent;
    while (walk != kNullNode) {
        walk = balance(walk);
        const int32_t child1 = m_nodes[static_cast<size_t>(walk)].child1;
        const int32_t child2 = m_nodes[static_cast<size_t>(walk)].child2;
        m_nodes[static_cast<size_t>(walk)].box =
            m_nodes[static_cast<size_t>(child1)].box.combined(m_nodes[static_cast<size_t>(child2)].box);
        m_nodes[static_cast<size_t>(walk)].height =
            1 + std::max(m_nodes[static_cast<size_t>(child1)].height, m_nodes[static_cast<size_t>(child2)].height);
        walk = m_nodes[static_cast<size_t>(walk)].parent;
    }
}

void DynamicAABBTree::removeLeaf(ProxyId proxyId) {
    removeLeafNode(proxyId);
    freeNode(proxyId);
    --m_leafCount;
}

void DynamicAABBTree::removeLeafNode(int32_t leaf) {
    if (leaf == m_root) {
        m_root = kNullNode;
        m_nodes[static_cast<size_t>(leaf)].parent = kNullNode;
        return;
    }
    const int32_t parent = m_nodes[static_cast<size_t>(leaf)].parent;
    const int32_t grandParent = m_nodes[static_cast<size_t>(parent)].parent;
    const int32_t sibling = m_nodes[static_cast<size_t>(parent)].child1 == leaf
                                ? m_nodes[static_cast<size_t>(parent)].child2
                                : m_nodes[static_cast<size_t>(parent)].child1;
    if (grandParent != kNullNode) {
        if (m_nodes[static_cast<size_t>(grandParent)].child1 == parent) {
            m_nodes[static_cast<size_t>(grandParent)].child1 = sibling;
        } else {
            m_nodes[static_cast<size_t>(grandParent)].child2 = sibling;
        }
        m_nodes[static_cast<size_t>(sibling)].parent = grandParent;
        freeNode(parent);
        // Refit and rebalance everything above the removed leaf.
        int32_t walk = grandParent;
        while (walk != kNullNode) {
            walk = balance(walk);
            const int32_t child1 = m_nodes[static_cast<size_t>(walk)].child1;
            const int32_t child2 = m_nodes[static_cast<size_t>(walk)].child2;
            m_nodes[static_cast<size_t>(walk)].box =
                m_nodes[static_cast<size_t>(child1)].box.combined(m_nodes[static_cast<size_t>(child2)].box);
            m_nodes[static_cast<size_t>(walk)].height =
                1 + std::max(m_nodes[static_cast<size_t>(child1)].height,
                             m_nodes[static_cast<size_t>(child2)].height);
            walk = m_nodes[static_cast<size_t>(walk)].parent;
        }
    } else {
        m_root = sibling;
        m_nodes[static_cast<size_t>(sibling)].parent = kNullNode;
        freeNode(parent);
    }
    m_nodes[static_cast<size_t>(leaf)].parent = kNullNode;
}

bool DynamicAABBTree::moveLeaf(ProxyId proxyId, const AABB& box, const glm::vec3& displacement) {
    const AABB& fat = m_nodes[static_cast<size_t>(proxyId)].box;
    if (fat.contains(box)) {
        // Small motion is free only while the displacement-predicted
        // reach also stays inside the fat box; a body accelerating
        // out of its prediction refits early instead of escaping.
        AABB predicted = box;
        const glm::vec3 reach = displacement * kDisplacementMultiplier;
        predicted.min += glm::min(reach, glm::vec3{0.0f});
        predicted.max += glm::max(reach, glm::vec3{0.0f});
        if (fat.contains(predicted)) {
            return false;
        }
    }
    removeLeafNode(proxyId);
    m_nodes[static_cast<size_t>(proxyId)].box = fatten(box, displacement);
    insertLeafNode(proxyId);
    return true;
}

int32_t DynamicAABBTree::balance(int32_t aIndex) {
    Node& a = m_nodes[static_cast<size_t>(aIndex)];
    if (a.isLeaf() || a.height < 2) {
        return aIndex;
    }
    const int32_t bIndex = a.child1;
    const int32_t cIndex = a.child2;
    const int32_t balanceFactor =
        m_nodes[static_cast<size_t>(cIndex)].height - m_nodes[static_cast<size_t>(bIndex)].height;

    if (balanceFactor > 1) {
        // C is too tall: rotate C up, keeping the shorter of C's
        // children under A (minimizes the resulting surface area).
        Node& c = m_nodes[static_cast<size_t>(cIndex)];
        const int32_t fIndex = c.child1;
        const int32_t gIndex = c.child2;
        Node& b = m_nodes[static_cast<size_t>(bIndex)];
        Node& f = m_nodes[static_cast<size_t>(fIndex)];
        Node& g = m_nodes[static_cast<size_t>(gIndex)];
        c.child1 = aIndex;
        c.parent = a.parent;
        a.parent = cIndex;
        if (c.parent != kNullNode) {
            if (m_nodes[static_cast<size_t>(c.parent)].child1 == aIndex) {
                m_nodes[static_cast<size_t>(c.parent)].child1 = cIndex;
            } else {
                m_nodes[static_cast<size_t>(c.parent)].child2 = cIndex;
            }
        } else {
            m_root = cIndex;
        }
        if (f.height > g.height) {
            c.child2 = fIndex;
            a.child2 = gIndex;
            g.parent = aIndex;
            a.box = b.box.combined(g.box);
            c.box = a.box.combined(f.box);
            a.height = 1 + std::max(b.height, g.height);
            c.height = 1 + std::max(a.height, f.height);
        } else {
            c.child2 = gIndex;
            a.child2 = fIndex;
            f.parent = aIndex;
            a.box = b.box.combined(f.box);
            c.box = a.box.combined(g.box);
            a.height = 1 + std::max(b.height, f.height);
            c.height = 1 + std::max(a.height, g.height);
        }
        return cIndex;
    }

    if (balanceFactor < -1) {
        // Mirror image: B is too tall, rotate B up.
        Node& b = m_nodes[static_cast<size_t>(bIndex)];
        const int32_t dIndex = b.child1;
        const int32_t eIndex = b.child2;
        Node& c = m_nodes[static_cast<size_t>(cIndex)];
        Node& d = m_nodes[static_cast<size_t>(dIndex)];
        Node& e = m_nodes[static_cast<size_t>(eIndex)];
        b.child1 = aIndex;
        b.parent = a.parent;
        a.parent = bIndex;
        if (b.parent != kNullNode) {
            if (m_nodes[static_cast<size_t>(b.parent)].child1 == aIndex) {
                m_nodes[static_cast<size_t>(b.parent)].child1 = bIndex;
            } else {
                m_nodes[static_cast<size_t>(b.parent)].child2 = bIndex;
            }
        } else {
            m_root = bIndex;
        }
        if (d.height > e.height) {
            b.child2 = dIndex;
            a.child1 = eIndex;
            e.parent = aIndex;
            a.box = c.box.combined(e.box);
            b.box = a.box.combined(d.box);
            a.height = 1 + std::max(c.height, e.height);
            b.height = 1 + std::max(a.height, d.height);
        } else {
            b.child2 = eIndex;
            a.child1 = dIndex;
            d.parent = aIndex;
            a.box = c.box.combined(d.box);
            b.box = a.box.combined(e.box);
            a.height = 1 + std::max(c.height, d.height);
            b.height = 1 + std::max(a.height, e.height);
        }
        return bIndex;
    }

    return aIndex;
}

int32_t DynamicAABBTree::computeHeight(int32_t nodeIndex) const {
    const Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
    if (node.isLeaf()) {
        return 0;
    }
    return 1 + std::max(computeHeight(node.child1), computeHeight(node.child2));
}

entt::entity DynamicAABBTree::entity(ProxyId proxyId) const {
    return m_nodes[static_cast<size_t>(proxyId)].entity;
}

const AABB& DynamicAABBTree::proxyAabb(ProxyId proxyId) const {
    return m_nodes[static_cast<size_t>(proxyId)].box;
}

int DynamicAABBTree::height() const {
    return m_root == kNullNode ? 0 : m_nodes[static_cast<size_t>(m_root)].height;
}

bool DynamicAABBTree::validate() const {
    if (m_root == kNullNode) {
        return m_leafCount == 0 && m_nodeCount == 0;
    }
    if (m_nodes[static_cast<size_t>(m_root)].parent != kNullNode) {
        return false;
    }
    size_t leaves = 0;
    size_t internals = 0;
    std::vector<int32_t> stack;
    stack.push_back(m_root);
    while (!stack.empty()) {
        const int32_t nodeIndex = stack.back();
        stack.pop_back();
        if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= m_nodes.size()) {
            return false;
        }
        const Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
        if (node.height < 0) {
            return false; // a freed node is reachable: corruption
        }
        if (node.isLeaf()) {
            if (node.height != 0) {
                return false;
            }
            ++leaves;
            continue;
        }
        ++internals;
        const int32_t child1 = node.child1;
        const int32_t child2 = node.child2;
        if (child1 < 0 || child2 < 0 || static_cast<size_t>(child1) >= m_nodes.size() ||
            static_cast<size_t>(child2) >= m_nodes.size() || child1 == nodeIndex || child2 == nodeIndex) {
            return false;
        }
        const Node& nodeChild1 = m_nodes[static_cast<size_t>(child1)];
        const Node& nodeChild2 = m_nodes[static_cast<size_t>(child2)];
        if (nodeChild1.parent != nodeIndex || nodeChild2.parent != nodeIndex) {
            return false;
        }
        if (node.height != 1 + std::max(nodeChild1.height, nodeChild2.height)) {
            return false;
        }
        if (!boxesEqual(node.box, nodeChild1.box.combined(nodeChild2.box))) {
            return false;
        }
        stack.push_back(child1);
        stack.push_back(child2);
    }
    if (leaves != m_leafCount || leaves + internals != m_nodeCount) {
        return false;
    }
    // Every allocated slot is either reachable (counted above) or on
    // the free list; the two must account for the whole pool.
    size_t freeCount = 0;
    int32_t freeIndex = m_freeNode;
    while (freeIndex != kNullNode) {
        if (freeIndex < 0 || static_cast<size_t>(freeIndex) >= m_nodes.size() || freeCount > m_nodes.size()) {
            return false;
        }
        ++freeCount;
        freeIndex = m_nodes[static_cast<size_t>(freeIndex)].next;
    }
    return freeCount + m_nodeCount == m_nodes.size();
}

void DynamicAABBTree::clear() {
    m_nodes.clear();
    m_root = kNullNode;
    m_freeNode = kNullNode;
    m_nodeCount = 0;
    m_leafCount = 0;
}

} // namespace engine

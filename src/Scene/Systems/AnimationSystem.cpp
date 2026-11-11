#include "Scene/Systems/AnimationSystem.h"

#include "Physics/PhysicsComponents.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace engine {

namespace {

// Euler (degrees, Y-X-Z order — the TransformComponent convention)
// to quaternion, for slerp-based rotation sampling.
[[nodiscard]] glm::quat quatFromEulerDegrees(const glm::vec3& degrees) {
    const glm::vec3 radians = glm::radians(degrees);
    return glm::normalize(glm::angleAxis(radians.y, glm::vec3{0.0f, 1.0f, 0.0f}) *
                          glm::angleAxis(radians.x, glm::vec3{1.0f, 0.0f, 0.0f}) *
                          glm::angleAxis(radians.z, glm::vec3{0.0f, 0.0f, 1.0f}));
}

[[nodiscard]] AnimPose poseFromKey(const AnimKeyframe& key) {
    AnimPose pose;
    pose.position = key.position;
    pose.rotation = quatFromEulerDegrees(key.rotationDegrees);
    pose.scale = key.scale;
    return pose;
}

[[nodiscard]] AnimPose mixPose(const AnimPose& from, const AnimPose& to, double factor) {
    const float f = static_cast<float>(std::clamp(factor, 0.0, 1.0));
    AnimPose pose;
    pose.position = glm::mix(from.position, to.position, f);
    pose.rotation = glm::slerp(from.rotation, to.rotation, f);
    pose.scale = glm::mix(from.scale, to.scale, f);
    return pose;
}

// Advances one player's playheads (current clip + outgoing blend
// clip). Non-looping clips stop at their end; looping ones wrap.
void advancePlayer(AnimationPlayerComponent& player, double deltaSeconds) {
    if (!player.playing) {
        return;
    }
    const AnimationClip* clip = player.findClip(player.currentClip);
    if (clip == nullptr) {
        player.playing = false;
        return;
    }
    player.time += deltaSeconds * static_cast<double>(player.speed);
    if (clip->duration > 0.0) {
        if (clip->loop) {
            player.time = std::fmod(player.time, clip->duration);
            if (player.time < 0.0) {
                player.time += clip->duration;
            }
        } else if (player.time >= clip->duration) {
            player.time = clip->duration;
            player.playing = false;
        }
    }
    if (!player.blendFromClip.empty() && player.blendDuration > 0.0) {
        player.blendElapsed += deltaSeconds;
        const AnimationClip* from = player.findClip(player.blendFromClip);
        if (from != nullptr && from->duration > 0.0) {
            player.blendFromTime += deltaSeconds * static_cast<double>(player.speed);
            if (from->loop) {
                player.blendFromTime = std::fmod(player.blendFromTime, from->duration);
            } else {
                player.blendFromTime = std::min(player.blendFromTime, from->duration);
            }
        }
        if (player.blendElapsed >= player.blendDuration) {
            player.blendFromClip.clear();
            player.blendDuration = 0.0;
            player.blendElapsed = 0.0;
        }
    }
}

// Writes the player's current pose (crossfade-aware) into the rig
// nodes' local transforms.
void applyPlayerPose(Scene& scene, entt::entity rigEntity, AnimationPlayerComponent& player) {
    auto& registry = scene.registry();
    const RigComponent* rig = registry.try_get<RigComponent>(rigEntity);
    const AnimationClip* clip = player.findClip(player.currentClip);
    if (rig == nullptr || clip == nullptr) {
        return;
    }
    const AnimationClip* fromClip = player.blendDuration > 0.0 ? player.findClip(player.blendFromClip) : nullptr;
    double blendFactor = 1.0;
    if (fromClip != nullptr && player.blendDuration > 0.0) {
        const double t = std::clamp(player.blendElapsed / player.blendDuration, 0.0, 1.0);
        blendFactor = t * t * (3.0 - 2.0 * t); // smoothstep crossfade
    }
    // Union of animated node names (current clip first).
    std::vector<const AnimTrack*> tracks;
    for (const AnimTrack& track : clip->tracks) {
        tracks.push_back(&track);
    }
    if (fromClip != nullptr) {
        for (const AnimTrack& track : fromClip->tracks) {
            bool present = false;
            for (const AnimTrack* existing : tracks) {
                present = present || existing->node == track.node;
            }
            if (!present) {
                tracks.push_back(&track);
            }
        }
    }
    for (const AnimTrack* track : tracks) {
        const RigNode* node = rig->findNode(track->node);
        if (node == nullptr || node->entity == entt::null || !registry.valid(node->entity)) {
            continue;
        }
        TransformComponent* transform = registry.try_get<TransformComponent>(node->entity);
        if (transform == nullptr) {
            continue;
        }
        AnimPose pose;
        const bool hasCurrent = sampleClip(*clip, track->node, player.time, pose);
        if (fromClip != nullptr) {
            AnimPose fromPose;
            if (sampleClip(*fromClip, track->node, player.blendFromTime, fromPose)) {
                pose = hasCurrent ? mixPose(fromPose, pose, blendFactor) : fromPose;
            } else if (!hasCurrent) {
                continue;
            }
        } else if (!hasCurrent) {
            continue;
        }
        transform->position = pose.position;
        transform->setOrientation(pose.rotation);
        transform->scale = pose.scale;
    }
}

void activateRagdoll(Scene& scene, entt::entity rigEntity) {
    auto& registry = scene.registry();
    RigComponent* rig = registry.try_get<RigComponent>(rigEntity);
    RagdollComponent* ragdoll = registry.try_get<RagdollComponent>(rigEntity);
    if (rig == nullptr || ragdoll == nullptr || ragdoll->active) {
        return;
    }
    ragdoll->bindTransforms.clear();
    ragdoll->bindTransforms.reserve(rig->nodes.size());
    for (const RigNode& node : rig->nodes) {
        const TransformComponent* transform =
            node.entity != entt::null && registry.valid(node.entity)
                ? registry.try_get<TransformComponent>(node.entity)
                : nullptr;
        ragdoll->bindTransforms.push_back(transform != nullptr ? *transform : TransformComponent{});
    }
    for (size_t index = 0; index < rig->nodes.size(); ++index) {
        const RigNode& node = rig->nodes[index];
        if (node.entity == entt::null || !registry.valid(node.entity)) {
            continue;
        }
        RagdollNodeComponent& marker = registry.get_or_emplace<RagdollNodeComponent>(node.entity);
        marker.rig = rigEntity;
        if (registry.try_get<RigidBodyComponent>(node.entity) == nullptr) {
            registry.emplace<RigidBodyComponent>(node.entity, ragdoll->bodyMass);
            marker.addedBody = true;
        }
        if (RigidBodyComponent* body = registry.try_get<RigidBodyComponent>(node.entity); body != nullptr) {
            body->sleeping = false;
            body->sleepTimer = 0.0f;
        }
        if (registry.try_get<ColliderComponent>(node.entity) == nullptr) {
            const glm::vec3 scale = ragdoll->bindTransforms[index].scale;
            ColliderComponent collider = ColliderComponent::capsule(
                std::max(0.05f, 0.18f * std::max(scale.x, scale.z)), std::max(0.05f, 0.18f * scale.y));
            registry.emplace<ColliderComponent>(node.entity, collider);
            marker.addedCollider = true;
        }
        if (node.parent >= 0 && registry.try_get<JointComponent>(node.entity) == nullptr) {
            const RigNode& parentNode = rig->nodes[static_cast<size_t>(node.parent)];
            if (parentNode.entity != entt::null && registry.valid(parentNode.entity)) {
                JointComponent joint = JointComponent::hinge(parentNode.entity, glm::vec3{0.0f},
                                                             ragdoll->bindTransforms[index].position, node.jointAxis);
                joint.enableLimits = true;
                joint.minAngleDegrees = node.minAngleDegrees;
                joint.maxAngleDegrees = node.maxAngleDegrees;
                joint.compliance = ragdoll->compliance;
                registry.emplace<JointComponent>(node.entity, joint);
                marker.addedJoint = true;
            }
        }
    }
    ragdoll->active = true;
}

void deactivateRagdoll(Scene& scene, entt::entity rigEntity) {
    auto& registry = scene.registry();
    RigComponent* rig = registry.try_get<RigComponent>(rigEntity);
    RagdollComponent* ragdoll = registry.try_get<RagdollComponent>(rigEntity);
    if (rig == nullptr || ragdoll == nullptr || !ragdoll->active) {
        return;
    }
    for (size_t index = 0; index < rig->nodes.size(); ++index) {
        const RigNode& node = rig->nodes[index];
        if (node.entity == entt::null || !registry.valid(node.entity)) {
            continue;
        }
        if (const RagdollNodeComponent* marker = registry.try_get<RagdollNodeComponent>(node.entity);
            marker != nullptr && marker->rig == rigEntity) {
            if (marker->addedJoint) {
                registry.remove<JointComponent>(node.entity);
            }
            if (marker->addedCollider) {
                registry.remove<ColliderComponent>(node.entity);
            }
            if (marker->addedBody) {
                registry.remove<RigidBodyComponent>(node.entity);
            }
            registry.remove<RagdollNodeComponent>(node.entity);
        }
        if (index < ragdoll->bindTransforms.size()) {
            if (TransformComponent* transform = registry.try_get<TransformComponent>(node.entity);
                transform != nullptr) {
                *transform = ragdoll->bindTransforms[index];
            }
        }
    }
    ragdoll->bindTransforms.clear();
    ragdoll->active = false;
}

} // namespace

double animEase(AnimInterpolation mode, double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    switch (mode) {
    case AnimInterpolation::Step:
        return 0.0; // hold the segment's start key until the next key
    case AnimInterpolation::EaseInOut:
        return clamped * clamped * (3.0 - 2.0 * clamped);
    case AnimInterpolation::Linear:
        break;
    }
    return clamped;
}

bool sampleTrack(const AnimTrack& track, double time, AnimPose& out) {
    if (track.keys.empty()) {
        return false;
    }
    const std::vector<AnimKeyframe>& keys = track.keys;
    if (time <= keys.front().time) {
        out = poseFromKey(keys.front());
        return true;
    }
    if (time >= keys.back().time) {
        out = poseFromKey(keys.back());
        return true;
    }
    size_t segment = 0;
    while (segment + 1 < keys.size() && keys[segment + 1].time <= time) {
        ++segment;
    }
    const AnimKeyframe& from = keys[segment];
    const AnimKeyframe& to = keys[segment + 1];
    const double span = to.time - from.time;
    const double local = span > 0.0 ? (time - from.time) / span : 0.0;
    out = mixPose(poseFromKey(from), poseFromKey(to), animEase(from.interpolation, local));
    return true;
}

bool sampleClip(const AnimationClip& clip, const std::string& node, double time, AnimPose& out) {
    const AnimTrack* track = clip.findTrack(node);
    if (track == nullptr) {
        return false;
    }
    double t = time;
    if (clip.duration > 0.0) {
        if (clip.loop) {
            t = std::fmod(t, clip.duration);
            if (t < 0.0) {
                t += clip.duration;
            }
        } else {
            t = std::clamp(t, 0.0, clip.duration);
        }
    }
    return sampleTrack(*track, t, out);
}

void addKeyframe(AnimationClip& clip, const std::string& node, const AnimKeyframe& key) {
    AnimTrack& track = clip.trackFor(node);
    std::vector<AnimKeyframe>& keys = track.keys;
    const auto existing = std::find_if(keys.begin(), keys.end(),
                                       [&key](const AnimKeyframe& k) { return std::abs(k.time - key.time) < 1e-9; });
    if (existing != keys.end()) {
        *existing = key;
    } else {
        const auto at = std::lower_bound(keys.begin(), keys.end(), key.time,
                                         [](const AnimKeyframe& k, double t) { return k.time < t; });
        keys.insert(at, key);
    }
    clip.duration = std::max(clip.duration, key.time);
}

bool removeKeyframe(AnimationClip& clip, const std::string& node, double time) {
    for (AnimTrack& track : clip.tracks) {
        if (track.node != node) {
            continue;
        }
        const auto at = std::find_if(track.keys.begin(), track.keys.end(),
                                     [time](const AnimKeyframe& k) { return std::abs(k.time - time) < 1e-9; });
        if (at == track.keys.end()) {
            return false;
        }
        track.keys.erase(at);
        return true;
    }
    return false;
}

void AnimationSystem::update(Scene& scene, double deltaSeconds) {
    auto& registry = scene.registry();
    // Ragdoll reconciliation first (bodies must exist before the
    // physics step in the same frame).
    std::vector<entt::entity> ragdollRoots;
    for (const entt::entity entity : registry.view<RigComponent, RagdollComponent>()) {
        ragdollRoots.push_back(entity);
    }
    for (const entt::entity entity : ragdollRoots) {
        RagdollComponent& ragdoll = registry.get<RagdollComponent>(entity);
        if (ragdoll.enabled && !ragdoll.active) {
            activateRagdoll(scene, entity);
        } else if (!ragdoll.enabled && ragdoll.active) {
            deactivateRagdoll(scene, entity);
        }
    }
    std::vector<entt::entity> players;
    for (const entt::entity entity : registry.view<RigComponent, AnimationPlayerComponent>()) {
        players.push_back(entity);
    }
    for (const entt::entity entity : players) {
        if (const RagdollComponent* ragdoll = registry.try_get<RagdollComponent>(entity);
            ragdoll != nullptr && ragdoll->active) {
            continue; // physics owns the nodes while ragdolled
        }
        AnimationPlayerComponent& player = registry.get<AnimationPlayerComponent>(entity);
        advancePlayer(player, deltaSeconds);
        applyPlayerPose(scene, entity, player);
    }
}

void AnimationSystem::startPreview(Scene& scene, AnimationContext& context, entt::entity rig) {
    auto& registry = scene.registry();
    if (context.previews.contains(rig)) {
        return;
    }
    AnimationPlayerComponent* player = registry.try_get<AnimationPlayerComponent>(rig);
    const RigComponent* rigComponent = registry.try_get<RigComponent>(rig);
    if (player == nullptr || rigComponent == nullptr) {
        return;
    }
    AnimationContext::PreviewState state;
    state.playback = *player;
    for (const RigNode& node : rigComponent->nodes) {
        if (node.entity != entt::null && registry.valid(node.entity)) {
            if (const TransformComponent* transform = registry.try_get<TransformComponent>(node.entity);
                transform != nullptr) {
                state.transforms.emplace_back(node.entity, *transform);
            }
        }
    }
    context.previews.emplace(rig, std::move(state));
    if (player->currentClip.empty() && !player->clips.empty()) {
        player->currentClip = player->clips.front().name;
    }
    player->time = 0.0;
    player->blendFromClip.clear();
    player->blendDuration = 0.0;
    player->playing = true;
}

void AnimationSystem::stopPreview(Scene& scene, AnimationContext& context, entt::entity rig) {
    const auto found = context.previews.find(rig);
    if (found == context.previews.end()) {
        return;
    }
    auto& registry = scene.registry();
    if (registry.valid(rig)) {
        if (AnimationPlayerComponent* player = registry.try_get<AnimationPlayerComponent>(rig); player != nullptr) {
            *player = found->second.playback;
        }
        for (const auto& [entity, transform] : found->second.transforms) {
            if (registry.valid(entity)) {
                if (TransformComponent* current = registry.try_get<TransformComponent>(entity); current != nullptr) {
                    *current = transform;
                }
            }
        }
    }
    context.previews.erase(found);
}

void AnimationSystem::stopAllPreviews(Scene& scene, AnimationContext& context) {
    std::vector<entt::entity> rigs;
    for (const auto& [rig, state] : context.previews) {
        rigs.push_back(rig);
    }
    for (const entt::entity rig : rigs) {
        stopPreview(scene, context, rig);
    }
}

bool AnimationSystem::previewActive(const AnimationContext& context, entt::entity rig) {
    return context.previews.contains(rig);
}

void AnimationSystem::updatePreview(Scene& scene, AnimationContext& context, double deltaSeconds) {
    std::vector<entt::entity> rigs;
    for (const auto& [rig, state] : context.previews) {
        rigs.push_back(rig);
    }
    auto& registry = scene.registry();
    for (const entt::entity rig : rigs) {
        AnimationPlayerComponent* player = registry.try_get<AnimationPlayerComponent>(rig);
        if (player == nullptr) {
            continue;
        }
        if (const RagdollComponent* ragdoll = registry.try_get<RagdollComponent>(rig);
            ragdoll != nullptr && ragdoll->active) {
            continue;
        }
        advancePlayer(*player, deltaSeconds);
        const AnimationClip* clip = player->findClip(player->currentClip);
        if (!player->playing && clip != nullptr && !clip->loop && player->time >= clip->duration) {
            player->time = 0.0; // non-looping clip ended: preview loops it (a manual pause has time < duration)
            player->playing = true;
        }
        applyPlayerPose(scene, rig, *player);
    }
}

bool AnimationSystem::playClip(Scene& scene, entt::entity rig, std::string_view clipName, double blendSeconds) {
    auto& registry = scene.registry();
    AnimationPlayerComponent* player = registry.try_get<AnimationPlayerComponent>(rig);
    if (player == nullptr || player->findClip(std::string(clipName)) == nullptr) {
        return false;
    }
    if (player->currentClip == clipName) {
        player->time = 0.0; // restart the running clip
        player->blendFromClip.clear();
        player->blendDuration = 0.0;
        player->playing = true;
        return true;
    }
    if (blendSeconds > 0.0 && !player->currentClip.empty() && player->findClip(player->currentClip) != nullptr) {
        player->blendFromClip = player->currentClip;
        player->blendFromTime = player->time;
        player->blendElapsed = 0.0;
        player->blendDuration = blendSeconds;
    } else {
        player->blendFromClip.clear();
        player->blendDuration = 0.0;
    }
    player->currentClip = std::string(clipName);
    player->time = 0.0;
    player->playing = true;
    return true;
}

void AnimationSystem::stopClip(Scene& scene, entt::entity rig) {
    auto& registry = scene.registry();
    if (AnimationPlayerComponent* player = registry.try_get<AnimationPlayerComponent>(rig); player != nullptr) {
        player->playing = false;
        player->currentClip.clear();
        player->blendFromClip.clear();
        player->blendDuration = 0.0;
    }
}

bool AnimationSystem::setRagdollEnabled(Scene& scene, entt::entity rig, bool enabled) {
    auto& registry = scene.registry();
    if (registry.try_get<RigComponent>(rig) == nullptr) {
        return false;
    }
    RagdollComponent& ragdoll = registry.get_or_emplace<RagdollComponent>(rig);
    ragdoll.enabled = enabled;
    if (enabled && !ragdoll.active) {
        activateRagdoll(scene, rig);
    } else if (!enabled && ragdoll.active) {
        deactivateRagdoll(scene, rig);
    }
    return true;
}

bool AnimationSystem::ragdollActive(Scene& scene, entt::entity rig) {
    auto& registry = scene.registry();
    const RagdollComponent* ragdoll = registry.try_get<RagdollComponent>(rig);
    return ragdoll != nullptr && ragdoll->active;
}

RigComponent AnimationRigging::buildFromHierarchy(Scene& scene, entt::entity root) {
    RigComponent rig;
    auto& registry = scene.registry();
    std::unordered_set<std::string> usedNames;
    const auto nameFor = [&registry, &usedNames](entt::entity entity) {
        std::string base = "node";
        if (const TagComponent* tag = registry.try_get<TagComponent>(entity); tag != nullptr && !tag->tag.empty()) {
            base = tag->tag;
        }
        std::string name = base;
        int suffix = 2;
        while (usedNames.contains(name)) {
            name = base + "_" + std::to_string(suffix++);
        }
        usedNames.insert(name);
        return name;
    };
    // Pre-order DFS so parents always precede their children.
    const std::function<void(entt::entity, int32_t)> walk = [&](entt::entity entity, int32_t parentIndex) {
        RigNode node;
        node.name = nameFor(entity);
        node.entity = entity;
        node.parent = parentIndex;
        rig.nodes.push_back(node);
        const int32_t myIndex = static_cast<int32_t>(rig.nodes.size()) - 1;
        if (const ChildrenComponent* children = registry.try_get<ChildrenComponent>(entity); children != nullptr) {
            for (const entt::entity child : children->children) {
                if (child != entt::null && registry.valid(child)) {
                    walk(child, myIndex);
                }
            }
        }
    };
    if (root != entt::null && registry.valid(root)) {
        walk(root, -1);
    }
    return rig;
}

} // namespace engine
